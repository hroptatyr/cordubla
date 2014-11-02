cordubla
========

No frills core dump file filter for linux.

If enabled core dumps are written to /tmp/core/<UID>/ and this files
is symlinked back into the cwd of the process crashing.  This means
all core dumps are gathered locally in a designated, easy-to-peruse
spot but the process owners still see the dump showing up where they'd
expect it.

There's practically no means of customising the behaviour other than
to hard-wire different settings in the code and rebuild.
