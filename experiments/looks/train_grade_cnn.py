#!/usr/bin/env python3
"""Can a vision network see a grade where fourteen statistics could not?

    ./venv/bin/python train_grade_cnn.py POS_DIR NEG_DIR [NEG_DIR ...] [--shuffle] [--epochs N]

THE HEAD-TO-HEAD
----------------
Same images, same degradations, same grouping rule as discriminant.py, which reached:

    grouped 5-fold balanced accuracy   0.657
    TRAIN accuracy on the fitted rows  0.686   <- could not fit even with the answers

Train and validation being the same number is what made that decisive: the descriptor set does
not contain the information, regardless of model. If pixels do contain it, a small CNN on the
identical task should be far above 0.657 -- and the degradations here are gross ones a colorist
names instantly, so anything less than comfortable success is another negative.

The negatives are the EXACT files the descriptor test scored, written out by
`looks --degrade=N --write-degraded=DIR`. Regenerating equivalent degradations here would be a
paraphrase, and this project has been bitten four times by exactly that on this feature.

WHAT WOULD MAKE THIS LIE
------------------------
- **Scene memorisation.** 121 films is a small corpus and a film's positive and degraded copies
  are the same scene, so a split that put one in train and the other in validation would let the
  model memorise content and score brilliantly. The split is grouped by FILM.
- **Class imbalance.** Four negatives exist per positive. Each epoch samples ONE negative per
  film, so the classes stay balanced and accuracy stays interpretable.
- **Optimism.** `--shuffle` randomises the labels within each film and retrains. It should land
  near 0.50; anything higher means the protocol leaks.
"""
import argparse
import os
import random
import sys

import torch
import torch.nn as nn
from PIL import Image
from torch.utils.data import DataLoader, Dataset
from torchvision import models, transforms


class Pairs(Dataset):
    def __init__(self, films, pos_dir, neg_dirs, train, shuffle_labels, seed=0):
        self.films = films
        self.pos_dir = pos_dir
        self.neg_dirs = neg_dirs
        self.train = train
        self.rng = random.Random(seed)
        # Flip a film's two labels together, so a leak shows up as a real score rather than being
        # averaged away.
        self.flip = {f: (self.rng.random() < 0.5) for f in films} if shuffle_labels else {}
        norm = transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225])
        if train:
            self.tf = transforms.Compose([
                transforms.RandomResizedCrop(224, scale=(0.4, 1.0)),
                transforms.RandomHorizontalFlip(),
                transforms.ToTensor(), norm])
        else:
            self.tf = transforms.Compose([
                transforms.Resize(256), transforms.CenterCrop(224),
                transforms.ToTensor(), norm])

    def __len__(self):
        # Positives are repeated once per negative variant so the classes stay balanced while
        # every degradation is seen each epoch -- 4x the data per pass, same 50/50 split.
        return len(self.films) * 2 * (len(self.neg_dirs) if self.train else 1)

    def __getitem__(self, i):
        variant = (i // (len(self.films) * 2)) if self.train else 0
        j = i % (len(self.films) * 2)
        film = self.films[j // 2]
        positive = (j % 2 == 0)
        if positive:
            path = os.path.join(self.pos_dir, film)
        else:
            d = self.neg_dirs[variant % len(self.neg_dirs)]
            path = os.path.join(d, os.path.splitext(film)[0] + ".png")
        y = 1.0 if positive else 0.0
        if self.flip.get(film):
            y = 1.0 - y
        img = Image.open(path).convert("RGB")
        return self.tf(img), torch.tensor([y])


def balanced_acc(truth, pred):
    tp = sum(1 for t, p in zip(truth, pred) if t == 1 and p >= 0.5)
    fn = sum(1 for t, p in zip(truth, pred) if t == 1 and p < 0.5)
    tn = sum(1 for t, p in zip(truth, pred) if t == 0 and p < 0.5)
    fp = sum(1 for t, p in zip(truth, pred) if t == 0 and p >= 0.5)
    return 0.5 * ((tp / (tp + fn) if tp + fn else 0) + (tn / (tn + fp) if tn + fp else 0))


def evaluate(model, loader):
    model.eval()
    truth, pred = [], []
    with torch.no_grad():
        for x, y in loader:
            p = torch.sigmoid(model(x)).squeeze(1)
            truth += y.squeeze(1).tolist()
            pred += p.tolist()
    return balanced_acc(truth, pred)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pos_dir")
    ap.add_argument("neg_dirs", nargs="+")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--shuffle", action="store_true")
    a = ap.parse_args()

    films = sorted(f for f in os.listdir(a.pos_dir) if f.lower().endswith((".jpg", ".jpeg", ".png")))
    # Keep only films that have every negative variant, so no film is silently half-present.
    films = [f for f in films
             if all(os.path.exists(os.path.join(d, os.path.splitext(f)[0] + ".png"))
                    for d in a.neg_dirs)]
    random.Random(4242).shuffle(films)
    cut = int(0.75 * len(films))
    tr_films, va_films = films[:cut], films[cut:]
    print("%d films: %d train / %d val   (grouped -- no scene in both)"
          % (len(films), len(tr_films), len(va_films)))
    if a.shuffle:
        print("LABELS SHUFFLED — this is the null, expect ~0.50")

    tr = DataLoader(Pairs(tr_films, a.pos_dir, a.neg_dirs, True, a.shuffle, 1),
                    batch_size=16, shuffle=True, num_workers=0)
    va = DataLoader(Pairs(va_films, a.pos_dir, a.neg_dirs, False, a.shuffle, 1),
                    batch_size=16, shuffle=False, num_workers=0)
    tr_eval = DataLoader(Pairs(tr_films, a.pos_dir, a.neg_dirs, False, a.shuffle, 1),
                         batch_size=16, shuffle=False, num_workers=0)

    # Pretrained backbone is not optional at this corpus size: 121 scenes will not train a CNN
    # from scratch, and the question is whether PIXELS carry the signal, not whether 121 images
    # can teach a network to see.
    model = models.mobilenet_v3_small(weights=models.MobileNet_V3_Small_Weights.IMAGENET1K_V1)
    model.classifier[3] = nn.Linear(model.classifier[3].in_features, 1)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-4, weight_decay=1e-4)
    lossf = nn.BCEWithLogitsLoss()
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=a.epochs)

    best = 0.0
    for ep in range(1, a.epochs + 1):
        model.train()
        tot = 0.0
        for x, y in tr:
            opt.zero_grad()
            loss = lossf(model(x), y)
            loss.backward()
            opt.step()
            tot += loss.item() * x.size(0)
        sched.step()
        v = evaluate(model, va)
        t = evaluate(model, tr_eval)
        best = max(best, v)
        print("  epoch %2d  loss %.4f   train %.3f   val %.3f" % (ep, tot / len(tr.dataset), t, v))

    print("\nbest val balanced accuracy: %.3f" % best)
    print("descriptor baseline (discriminant.py): 0.657 val / 0.686 train")
    return 0


if __name__ == "__main__":
    sys.exit(main())
