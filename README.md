silentjack - silence detector for JACK
======================================

This is a fork of silentjack that adds OSC functions.

Please visit

* http://www.aelius.com/njh/silentjack/

and see the README file.

Usage
-----

```
silentjack -h
silentjack version 0.3 OSC

Usage: silentjack [options] [COMMAND [ARG]...]
Options:  -c <port>   Connect to this port
          -n <name>   Name of this client (default 'silentjack')
          -l <db>     Trigger level (default -40 decibels)
          -p <secs>   Period of silence required (default 1 second)
          -d <db>     No-dynamic trigger level (default disabled)
          -P <secs>   No-dynamic period (default 10 seconds)
          -g <secs>   Grace period (default 0 seconds)
          -v          Enable verbose mode
          -q          Enable quiet mode
          -o <port>   Set OSC Port for listening (default 7777, ? for random)
          -H <port>   Set OSC Host to send to (default 127.0.0.1)
          -O <port>   Set OSC Port to send to (default 7778)
          -V          Enable OSC verbose mode
          -X          Disable all OSC
          -h          Show help
```

Install on Linux
----------------

needs: autoconf, automake, JACK, liblo

```
  git clone git://github.com/7890/silentjack_osc.git
  cd silentjack_osc
  ./autogen.sh
  make && sudo make install
```
