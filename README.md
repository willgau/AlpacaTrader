# cppTrader

Repo to do benchmark on fast way to trade using Alpaca paper market.
The main goal is to focus on the fastest way to propagate information to the Alpaca call.

January 5 2026
First result:
Messages   : 5000000
Msg size   : 40 bytes
Time       : 0.115895 s
Throughput : 4.31426e+07 msg/s
Bandwidth  : 1645.76 MiB/s
Consumed   : 5000000
Checksum   : 7608582238643779454

Latency (ns) over 5000000 samples:
  min   : 0
  p50~  : 524288 (bucket upper bound)
  p99~  : 16777216 (bucket upper bound)
  p99.9~: 16777216 (bucket upper bound)
  max   : 12656200
  avg   : 1.0163e+06