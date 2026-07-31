# capslock-daemon

While Caps Lock is on, your Mac will not sleep. Turn it off and normal sleep behaviour returns.

The Caps Lock LED is the status indicator — there is no menu bar icon, no window, and no app.

195 lines of C. 35 KB binary. Measured at **0.008% of one core and 4.8 MB resident** (CPU-time delta over a 120 s steady-state window, so startup cost is excluded).

```
Caps Lock ON   ->  SleepDisabled = 1   (stays awake, even with the lid closed)
Caps Lock OFF  ->  SleepDisabled = 0   (normal sleep)
```

## Why you might want this

Long-running work — a build, a training run, an agent session — dies when the lid closes. The usual fixes are a GUI app in the menu bar, or remembering to run `caffeinate` and remembering to kill it. This makes it a physical toggle with a hardware indicator light.

## Design

### Detection is polled, not hooked

macOS can hook key input via `CGEventTap` or `NSEvent` global monitors. For reading one lock bit, that is a bad trade:

- an event tap **inserts itself into the path of every system-wide input event**
- it requires Input Monitoring — and if the binary lives in a Nix store path, the TCC grant is keyed to a path that changes on every rebuild, so it needs re-approving each time

`IOHIDGetModifierLockState()` on `IOHIDSystem` reads the same bit with **no permission at all**, and costs one syscall per poll.

One dead end worth recording: the IORegistry property `HIDCapsLockState` is readable as a normal user and looks like a third option. It is a **stale cache** — measured reporting `OFF` while the live state went `ON`. Do not use it.

### Sleep is blocked with SleepDisabled, not an assertion

`caffeinate` takes an `IOPMAssertion`. Assertions **do not survive the lid closing**, which is the entire use case here.

`SleepDisabled` — the system power setting `pmset -a disablesleep` writes — does. It is root-only, so this runs as a LaunchDaemon and calls `IOPMSetSystemPowerSetting` directly rather than spawning `pmset`. That SPI is undeclared in the public SDK, so it is weak-linked with a `pmset` fallback rather than assumed present.

The write lands in the IORegistry asynchronously. Verifying it with a single immediate read-back produces false failures under rapid toggling, so the check waits for a settle window. `pmset` also **exits 0 even when it refuses the write for lack of privilege**, so its exit status cannot be used as a success signal — hence an explicit `geteuid()` check plus read-back verification.

### Failure behaviour

- Refuses to start as non-root with a clear message rather than running as a no-op
- Sets `SleepDisabled=0` on startup, then reconciles with the live Caps Lock state — a crash-restart cannot leave the machine unable to sleep
- Restores `SleepDisabled=0` on `SIGTERM`/`SIGINT`
- `KeepAlive` in the plist, so launchd restarts it if it dies

## Install

Requires the Xcode command line tools (`xcode-select --install`).

```sh
git clone https://github.com/TetsuroAndo/capslock-daemon.git
cd capslock-daemon
make
sudo make install
```

That installs the binary to `/usr/local/libexec/capslock-daemon` and a LaunchDaemon to `/Library/LaunchDaemons/com.tetsuroando.capslock-daemon.plist`, then loads it.

Check it:

```sh
tail -f /var/log/capslock-daemon.log
pmset -g | grep -i sleepdisabled
```

Uninstall:

```sh
sudo make uninstall
```

### Nix

```nix
{
  inputs.capslock-daemon.url = "github:TetsuroAndo/capslock-daemon";

  # in your nix-darwin configuration
  imports = [ inputs.capslock-daemon.darwinModules.default ];
}
```

## Caveat

**Caps Lock is genuinely on while this is armed, so typing produces capitals.**

Suppressing that requires a `CGEventTap` to swallow the modifier, which means Accessibility permission and putting a tap in the system input path — the two things this deliberately avoids. If you need Caps Lock to keep working as Caps Lock, this is not the tool for you.

## License

MIT
