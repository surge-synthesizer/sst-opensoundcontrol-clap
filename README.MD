OSC adapter for Clap based plugins

Very early at this stage, do not use for production.

There are various issues :

- The oscpkt library used for OSC communications is outdated
- A spinlock is used (from the Choc library)
- Dependency on the Clap C++ helper library
- Only one kind of parameter change OSC message supported at the moment
- No outbound communications of any kind, this only receives OSC messages at the moment
and doesn't notify the Clap host application of parameter changes
- Error handling is not adequate

