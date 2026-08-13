#!/usr/bin/env python3
"""Can the descriptor set see a GRADE at all? A falsification test.

    ./looks MODEL film --frame-csv            > clean.csv
    ./looks MODEL film --frame-csv --degrade=1 > deg.csv
    ./discriminant.py clean.csv deg.csv

WHY THIS DESIGN
---------------
Separability was tested one axis at a time, and a class can be perfectly separable in a
COMBINATION of features while no single feature separates at all. So "nothing crossed the bar on
18 axes" does not show the information is absent, only that no axis carries it alone. This asks
the multivariate question.

It compares each still against a DEGRADED COPY OF ITSELF rather than against other footage.
Comparing film stills to OneGrade output cannot answer it: those are different scenes, so a
separator could be reading content and would score brilliantly while measuring nothing about
grading.

READ THE RESULT ASYMMETRICALLY. Failing means the descriptors cannot see grade at all, which is
decisive. Passing means only that they notice a shifted exposure -- a far lower bar than noticing
whether something looks cinematic, which is the actual question and which this cannot answer.

TWO WAYS THIS WOULD LIE, BOTH GUARDED
-------------------------------------
- Content leakage: a film's clean and degraded rows must never straddle a train/test split, or the
  model can memorise the scene. CV folds are grouped by FILE.
- Optimism: with 14 features and 242 rows a model can score well on noise. The null shuffles the
  clean/degraded label WITHIN each pair, which preserves everything except the distinction under
  test, and re-runs the whole pipeline.
"""
import csv
import math
import random
import sys


def load(path, label):
    rows = []
    with open(path) as f:
        for r in csv.reader(f):
            if not r or r[0] == "look":
                continue
            rows.append((r[1], [float(x) for x in r[2:]], label))
    return rows


def standardize(train, test):
    n = len(train[0])
    mu, sd = [], []
    for j in range(n):
        col = [r[j] for r in train if r[j] > -0.5]
        m = sum(col) / len(col) if col else 0.0
        v = sum((x - m) ** 2 for x in col) / len(col) if col else 1.0
        mu.append(m)
        sd.append(math.sqrt(v) or 1.0)

    def apply(rows):
        out = []
        for r in rows:
            out.append([((r[j] if r[j] > -0.5 else mu[j]) - mu[j]) / sd[j] for j in range(n)])
        return out

    return apply(train), apply(test)


def fit(X, y, iters=3000, lr=0.08, l2=0.02):
    n, d = len(X), len(X[0])
    w, b = [0.0] * d, 0.0
    for _ in range(iters):
        gw, gb = [0.0] * d, 0.0
        for i in range(n):
            z = b + sum(w[j] * X[i][j] for j in range(d))
            p = 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))
            e = p - y[i]
            for j in range(d):
                gw[j] += e * X[i][j]
            gb += e
        for j in range(d):
            w[j] -= lr * (gw[j] / n + l2 * w[j])
        b -= lr * gb / n
    return w, b


def predict(w, b, x):
    z = b + sum(w[j] * x[j] for j in range(len(x)))
    return 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))


def balanced_accuracy(truth, pred):
    tp = sum(1 for t, p in zip(truth, pred) if t == 1 and p >= 0.5)
    fn = sum(1 for t, p in zip(truth, pred) if t == 1 and p < 0.5)
    tn = sum(1 for t, p in zip(truth, pred) if t == 0 and p < 0.5)
    fp = sum(1 for t, p in zip(truth, pred) if t == 0 and p >= 0.5)
    sens = tp / (tp + fn) if tp + fn else 0.0
    spec = tn / (tn + fp) if tn + fp else 0.0
    return 0.5 * (sens + spec)


def run_cv(rows, files, folds=5):
    """Grouped by file: a film's two rows always land in the same fold."""
    order = sorted(files)
    random.Random(12345).shuffle(order)
    assign = {f: i % folds for i, f in enumerate(order)}
    truth, pred = [], []
    for k in range(folds):
        tr = [r for r in rows if assign[r[0]] != k]
        te = [r for r in rows if assign[r[0]] == k]
        if not te or len({r[2] for r in tr}) < 2:
            continue
        Xtr, Xte = standardize([r[1] for r in tr], [r[1] for r in te])
        w, b = fit(Xtr, [r[2] for r in tr])
        for x, r in zip(Xte, te):
            truth.append(r[2])
            pred.append(predict(w, b, x))
    return balanced_accuracy(truth, pred)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rows = load(sys.argv[1], 0) + load(sys.argv[2], 1)
    files = {r[0] for r in rows}
    print("%d rows, %d films, %d features" % (len(rows), len(files), len(rows[0][1])))

    real = run_cv(rows, files)
    print("\nGrouped 5-fold balanced accuracy: %.3f" % real)

    # NULL: swap the label within each pair at random. Everything else is identical, so whatever
    # this scores is what the pipeline achieves on no signal.
    print("\nPermutation null (label swapped within each pair)...")
    null = []
    for t in range(30):
        rng = random.Random(1000 + t)
        flip = {f: rng.random() < 0.5 for f in files}
        shuffled = [(f, x, (1 - y) if flip[f] else y) for f, x, y in rows]
        null.append(run_cv(shuffled, files))
    null.sort()
    med = null[len(null) // 2]
    p95 = null[int(0.95 * (len(null) - 1))]
    print("  null median %.3f   null 95th %.3f   null max %.3f" % (med, p95, null[-1]))
    print("\n  real %.3f vs null 95th %.3f -> %s"
          % (real, p95, "SIGNAL" if real > p95 else "NOT DISTINGUISHABLE FROM CHANCE"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
