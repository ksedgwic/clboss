## Auditing Onchain Fee Monitoring

### Purpose

CLBOSS monitors the onchain fee "weather" to control when expensive
operations are undertaken.  Specifically, CLBOSS waits for "low
onchain fees" before undertaking operations which pay onchain fees:
- opening channels
- closing channels
- creating swaps to generate incoming liquidity

### Procedures

Use the following command to see the results of the OnchainFeeMonitor:
```
sudo grep OnchainFeeMonitor ~cln/.lightning/bitcoin/lightning.log
```

Example Output:
```
2024-09-20T08:06:49.106Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (581, 583, 584): 530: low fees.
2024-09-20T08:16:49.142Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (581, 583, 584): 530: low fees.
2024-09-20T08:26:49.139Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (581, 583, 584): 530: low fees.
2024-09-20T08:36:49.149Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (581, 583, 584): 530: low fees.
2024-09-20T08:46:49.150Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (581, 583, 584): 530: low fees.
2024-09-20T08:56:49.365Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 530: low fees.
2024-09-20T09:06:49.253Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 530: low fees.
2024-09-20T09:16:49.163Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 613: high fees.
2024-09-20T09:26:49.204Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 588: high fees.
2024-09-20T09:36:49.171Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 588: high fees.
2024-09-20T09:46:49.178Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 588: high fees.
2024-09-20T09:56:49.161Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 588: high fees.
2024-09-20T10:06:49.188Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 587: high fees.
2024-09-20T10:16:49.177Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 588: high fees.
2024-09-20T10:26:49.179Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 588: high fees.
```

The output shows the OnchainFeeMonitor running periodically and determining the current fee rate:
```
2024-09-20T09:16:49.163Z DEBUG   plugin-clboss: OnchainFeeMonitor: Periodic: (580, 583, 584): 613: high fees.
                                                                              ^^^  ^^^  ^^^   ^^^  ^^^^^^^^^
                                                        threshold high-to-low -+    |    |     |       |
                                                           threshold midpoint ------+    |     |       |
                                                        threshold low-to-high -----------+     |       |
                                                             current fee rate -----------------+       |
                                                                     decision -------------------------+
```

### References
