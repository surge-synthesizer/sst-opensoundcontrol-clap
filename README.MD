OSC adapter for Clap based plugins

Very early at this stage, do not use for production.

There are various issues :

- The oscpkt library used for OSC communications is outdated
- A spinlock is used (from the Choc library) to protect the Clap event list
- Some other dependencies on Choc (text processing)
- Dependency on the Clap C++ helper library (for the event list). Perhaps not a big deal since
C++ Clap plugins are likely to use that anyway.
- Not many OSC message types handled
- No outbound communications, this only receives OSC messages at the moment
- Error handling is not adequate
