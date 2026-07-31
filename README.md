# capslock-daemon

[![ci](https://github.com/TetsuroAndo/capslock-daemon/actions/workflows/ci.yml/badge.svg)](https://github.com/TetsuroAndo/capslock-daemon/actions/workflows/ci.yml)

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

### Nix

```nix
{
  inputs.capslock-daemon.url = "github:TetsuroAndo/capslock-daemon";

  # in your nix-darwin configuration
  imports = [ inputs.capslock-daemon.darwinModules.default ];
}
```

Note that the Nix module registers the daemon under the label `org.nixos.capslock-daemon`, not `com.tetsuroando.capslock-daemon`. The `make` targets below do not apply to a Nix install — see [Turning it off](#turning-it-off).

## Turning it off

Stopping the daemon always restores `SleepDisabled=0` on the way out, so you never end up with a machine that refuses to sleep because you turned this off at the wrong moment.

Installed with `make` (label `com.tetsuroando.capslock-daemon`):

```sh
sudo make disable      # stop now, and stay stopped across reboots
sudo make enable       # start it again
sudo make uninstall    # remove the binary and the LaunchDaemon entirely
```

Installed via the Nix module (label `org.nixos.capslock-daemon`) — `make` does not know about this install, so use `launchctl` directly:

```sh
sudo launchctl bootout system/org.nixos.capslock-daemon     # stop until the next boot or switch
sudo launchctl disable system/org.nixos.capslock-daemon     # also stay stopped across reboots
sudo launchctl enable system/org.nixos.capslock-daemon      # undo the above
```

To remove it for good under Nix, drop `capslock-daemon.darwinModules.default` from your configuration and run your usual `darwin-rebuild switch`. That is the only removal the Nix declaration will not undo the next time you switch.

Which label you have:

```sh
ls /Library/LaunchDaemons | grep capslock
```

## Caveat

**Caps Lock is genuinely on while this is armed, so typing produces capitals.**

Suppressing that requires a `CGEventTap` to swallow the modifier, which means Accessibility permission and putting a tap in the system input path — the two things this deliberately avoids. If you need Caps Lock to keep working as Caps Lock, this is not the tool for you.

## License

MIT
