# Multithreaded T20 Cricket World Cup Simulator

A full two-innings T20 match (India vs England) simulated with POSIX
threads in C++17. Every player on the pitch is a live thread and every
ball is a synchronised event, so the match doubles as a demonstration of
operating-system concepts.

| Cricket role | OS concept |
|---|---|
| Batsman | Thread waiting on a resource (the ball) |
| Bowler | Thread producing a shared resource |
| Fielders | Ten threads competing for the same event |
| Umpire | Monitor thread running the deadlock detector (Banker's Algorithm) |
| Scheduler | Bowler rotation by FCFS, SJF or priority scheduling |
| Logger / commentator | Consumers on bounded event queues |

## Build and run

The source lives in [`multithreaded-Cricket_WC_simulater-main/`](multithreaded-Cricket_WC_simulater-main/).

```bash
cd multithreaded-Cricket_WC_simulater-main
make                              # builds ./t20_simulator
./t20_simulator                   # runs FCFS, SJF and priority in three child processes
./t20_simulator --sjf             # a single scheduling algorithm
./t20_simulator --deadlock-demo   # high-contention mode that triggers deadlock detection
make analysis                     # Gantt charts and statistics (Python, matplotlib)
```

Requires `g++` with C++17 and pthreads; Python 3.8+ with `matplotlib`,
`pandas` and `numpy` for the analysis scripts only.

## Documentation

The [full README](multithreaded-Cricket_WC_simulater-main/README.md)
covers the thread design, synchronisation primitives, scheduling
algorithms, event system, deadlock detection and probability models.
