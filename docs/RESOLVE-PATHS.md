# Where DaVinci Resolve keeps things

Written down after three macOS install failures in a row, each of which was diagnosed by
looking in one of these places and none of which was diagnosable without them. Resolve says
quite a lot about what it is doing; almost none of it reaches the UI.

## The support directory

Everything below lives under one root:

| platform | path |
|---|---|
| macOS | `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/` |
| Windows | `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\` |
| Linux | `~/.local/share/DaVinciResolve/` |

## `logs/davinci_resolve.log` — the first place to look

Resolve logs every plugin it loads, fails to load, or is asked for and does not have. The
three lines that matter:

```
OpenFX | INFO | OFX: loading com.mattgrdinic.OneGrade          <- found and loaded
OpenFX | INFO | Failed to load /Library/OFX/Plugins/OneGrade.ofx.bundle
OpenFX | WARN | Plugin [com.mattgrdinic.onegrade] is not available!!!!
```

The third is **not** a load failure. It means a project references the plugin and the host does
not have it — so seeing it *without* a nearby `loading` or `Failed to load` line means Resolve
never even attempted the bundle, which is a completely different problem from the bundle being
broken.

```bash
grep -i "ofx\|onegrade" ~/Library/Application\ Support/Blackmagic\ Design/DaVinci\ Resolve/logs/davinci_resolve.log | tail -30
```

## `OFXPluginCacheV2.xml` — the one that traps people

Resolve caches a verdict per plugin so it does not rescan every launch:

```xml
<plugin bundle_path="/Library/OFX/Plugins/OneGrade.ofx.bundle"
        path=".../OneGrade.ofx" mtime="0" size="0" status="2" />
```

`status="2"` is a failure. **A failed load is written with `mtime="0" size="0"`**, so Resolve
has no way to notice the file has changed and never tries again — the plugin stays invisible
through any number of reinstalls, and the log shows no attempt at all.

So a broken install does not merely fail: it *poisons the host* until the cache is cleared.
Deleting the file is safe; Resolve rebuilds it by rescanning on next launch.

```bash
# quit Resolve first
rm ~/Library/Application\ Support/Blackmagic\ Design/DaVinci\ Resolve/OFXPluginCacheV2.xml
```

`install/install-macos.command` now does this as part of installing, precisely because
reinstalling was otherwise incapable of fixing a bad install.

## Where plugins go

| platform | path |
|---|---|
| macOS | `/Library/OFX/Plugins/` |
| Windows | `%CommonProgramFiles%\OFX\Plugins\` |
| Linux | `/usr/OFX/Plugins/` |

## Where Resolve's LUTs live

The Film Look stocks OneGrade uses come from here, and the path is **not** the same shape on
every platform — Windows has an extra `Support` level, which was hardcoded wrong once and gave
an empty Film list with no error. See `filmLutDir()`.

| platform | path |
|---|---|
| macOS | `/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT/` |
| Windows | `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\LUT\` |
| Linux | `/opt/resolve/LUT/` |

## Diagnosing a plugin that will not appear

In order, because each step rules out everything below it:

1. **Is the bundle quarantined?** `xattr -lr <bundle>` — anything downloaded through a browser
   is, and Gatekeeper will not let Resolve load an unsigned quarantined bundle.
2. **Does it load at all?** `dlopen` it directly and call `OfxGetNumberOfPlugins`. If that
   works, the binary is fine and the problem is the host, not the plugin — which is the
   moment to stop looking at the code.
3. **Did Resolve try?** Check the log for `loading` or `Failed to load` naming the bundle.
4. **Is it cached as bad?** Check `OFXPluginCacheV2.xml` for `status="2"`.

Step 2 is the pivot. It is the difference between debugging a build and debugging an install,
and getting there early would have saved most of an afternoon.
