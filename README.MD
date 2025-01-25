OSC (Open Sound Control) adapter for Clap based plugins

Very early at this stage, do not use for production.

There are various issues :

- The oscpkt library used for OSC communications is outdated. (But does this actually matter? Need to investigate...)
- OSC addresses for plugin parameters are auto-generated from the plugin parameter display names, which
should not be considered stable. Hopefully the Clap plugin format itself gets an extension for this at some point or some other solution is implemented.
- Not many OSC message types handled (depending on the definition of "many")
- Error handling is not adequate
- No simple example Clap plugin that shows how to wire up with this
- No automated tests. (Tricky to do anyway because this would typically be used with external hardware/network I/O.)
