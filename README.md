# Pagemon

Pagemon is an interactive memory/page monitoring tool allowing one to browse the memory map of an active running process.

pagemon command line options:

* -h help
* -a enable automatic zoom mode
* -d delay in microseconds between refreshes, default 15000
* -p specify process ID of process to monitor
* -r read (page back in) pages at start
* -t specify ticks between dirty page checks
* -z set page zoom scale 

## Examples:

* [viewing an ARM64 QEMU virtual machine running](https://www.youtube.com/embed/AS0s5nl_IXY)
* [viewing page activity on a process that is sorting data](https://www.youtube.com/embed/Wq8YtKvC-Rw)
* `pagemon -avp $(pgrep -la "COMMANDNAME" | fzf --exact --height=~70% --border | awk '{print $1}')`
