## Auditing Channel Fee Setting

### Purpose

### Procedures

Use the following command to see the fee modification algorithm for a particular peer:
```
sudo grep FeeModder ~cln/.lightning/bitcoin/lightning.log | grep 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844
```

Example Output:
```
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T07:18:18.043Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T07:18:21.716Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 set to bin 0 of 25 due to balance 23303725msat / 5000000000msat: 6.53891
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T08:42:53.665Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T10:06:32.634Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T11:15:29.034Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T11:49:28.581Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T12:43:25.253Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T13:59:05.743Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T15:27:07.153Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T16:24:01.660Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T17:06:28.793Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T18:30:38.320Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T19:34:29.621Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T20:38:53.205Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T21:16:08.166Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T22:20:52.213Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T22:20:52.465Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 0 to bin 0 of 25 due to balance 293441478msat / 5000000000msat: 6.53891
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T23:37:57.248Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T23:37:57.979Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 0 to bin 1 of 25 due to balance 394721256msat / 5000000000msat: 5.59174
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-19T23:38:23.921Z INFO    plugin-clboss: FeeModderBySize: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 has 82 other peers, 14 of which have less capacity than us, 68 have more.  Multiplier: 0.651441
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T00:24:53.987Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T00:58:40.654Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T01:41:58.444Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T02:15:17.943Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T02:15:18.164Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 1 to bin 1 of 25 due to balance 402273006msat / 5000000000msat: 5.59174
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T03:18:23.216Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T03:18:23.732Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 1 to bin 1 of 25 due to balance 409027500msat / 5000000000msat: 5.59174
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T03:59:38.627Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T04:53:32.033Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T04:53:32.328Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 1 to bin 1 of 25 due to balance 410537739msat / 5000000000msat: 5.59174
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T06:17:00.823Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log-20240920:2024-09-20T06:17:00.921Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 1 to bin 1 of 25 due to balance 417237264msat / 5000000000msat: 5.59174
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T07:04:31.754Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T08:10:11.373Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000; measuring around new level -1 for new mesurement round; set to new level for this measurement round
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T08:10:11.676Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 1 to bin 1 of 25 due to balance 422657294msat / 5000000000msat: 5.59174
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T08:57:39.682Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T08:57:40.063Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 1 to bin 2 of 25 due to balance 431427086msat / 5000000000msat: 4.78176
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T10:18:45.857Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T11:40:17.888Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T13:04:02.686Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T14:29:29.690Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T15:01:41.945Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T16:19:26.620Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T17:32:02.476Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T18:15:19.253Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T19:39:57.526Z DEBUG   plugin-clboss: FeeModderByPriceTheory: 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844: level = -1, mult = 0.800000
/home/cln/.lightning/bitcoin/lightning.log:2024-09-20T19:39:57.874Z DEBUG   plugin-clboss: FeeModderByBalance: Peer 02e9046555a9665145b0dbd7f135744598418df7d61d3660659641886ef1274844 moved from bin 2 to bin 3 of 25 due to balance 763815161msat / 5000000000msat: 4.08911
```

### References
