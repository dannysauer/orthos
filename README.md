orthos
======

Orthos watches a Subversion export on Linux and logs local files that differ from SVN HEAD.

## Watch an export

Build Orthos from the repository root with `make`. You'll need a C11 compiler,
`pkg-config`, and the Subversion 1.12 or newer client development files
(`libsvn-dev` on Debian and Ubuntu). Use a client library that still gets
security updates from your distribution. Orthos also needs `/proc/self/fd`
to read and watch the same files it inspected.

Give Orthos the directory your exporter writes to and the URL it exports from.
For example, if your exporter copies `trunk` to `/srv/export`, run this in the
foreground with your own directory and URL:

```sh
build/orthos /srv/export https://svn.example.org/repos/project/trunk
```

If your exporter switches releases by replacing a parent directory or a symlink,
give Orthos the path your deployment uses to select the current release. Keep
that path under the deployer's control. While idle, Orthos checks once a second
whether it still points to the directory it scanned.

Run it as a user who can read the export and access the repository without a
password prompt. Orthos keeps a reference copy under `/tmp`, so leave room
there for one full export. It skips SVN externals. Use the same keyword and
line-ending settings for both exports or the file contents can differ.

## Read the alerts

A matching tree is quiet. To check the output on a disposable export, run
`mktemp /srv/export/orthos-check.XXXXXX` in a second terminal, substituting
your export directory. Leave the file there until Orthos prints a line like this:

```text
r42 unexpected /srv/export/orthos-check.Np7cX2
```

Remove the temporary file using the path `mktemp` printed. Orthos writes each
alert to stdout and flushes the line so another process can read it right away.
The first field gives the revision it checked. The reason can be `unexpected`,
`missing`, `contents differ`, `type differs`, `link differs`, or `unverifiable`. The rest of the
line is the absolute path, which can contain spaces. Orthos escapes backslashes
and control bytes as `\xNN`.

Orthos reports a discrepancy once. It logs another line if the reason changes,
or if a later check finds the path back in sync and the discrepancy comes back.
If Orthos can't read a path or watch a directory, it reports `unverifiable`,
continues checking the rest of the export, and retries within five seconds of
finishing the scan. If it can't write an alert, it logs the error to stderr
and exits with a failure status. Restore the log destination and restart
Orthos to check the export again.

If a repository check fails, look on stderr for the SVN error. Orthos waits five
seconds before trying again.

## When the comparison runs

Orthos checks the whole tree at startup. After that, local filesystem events
trigger checks. A commit by itself won't wake the watcher. Each check uses
one revision of HEAD and compares file contents and symbolic link targets.
Orthos doesn't compare timestamps or permission bits.

This works best for small exports that change infrequently. Give the exporter
time to finish and Orthos time to check its changes before committing another
change to those paths. An alert during an export can mean Orthos saw a partially
updated tree or that HEAD already moved on. Use the revision in the alert to
see what Orthos expected.

## Run the tests

Run `make check` from the repository root. It builds Orthos and runs the
integration tests with Python 3, `svn`, and `svnadmin`. Each test gets its own
temporary local repository. Run as an ordinary user to include the file
permission check.

[Apache License 2.0](LICENSE)
