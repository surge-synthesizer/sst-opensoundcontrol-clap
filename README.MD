OSC (Open Sound Control) adapter for Clap based plugins

Very early at this stage, do not use for production.

There are various issues :

- The oscpkt library used for OSC communications is outdated. (But does this actually matter? Need to investigate...)
- A spinlock is used (from the Choc library) to protect the Clap event lists
- Dependency on the Clap C++ helper library (for the event list). Perhaps not a big deal since
C++ Clap plugins are likely to use that anyway.
- Not many OSC message types handled
- Error handling is not adequate
- No simple example Clap plugin that shows how to wire up with this
