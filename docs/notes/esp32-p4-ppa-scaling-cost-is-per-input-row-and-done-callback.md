---
title: ESP32-P4 PPA scaling: cost is per input row, and done-callback fires for blocking calls too
date: 2026-11-09
tags: ppa,esp32-p4,performance
---

PPA SRM time scales with rows read: 1920-wide RGB565 to 600x475 RGB888 took 80.5 ms for 1520 rows, 43.1 for 760, 27.7 for 380 (measured on rev 3.2). Read every Nth row by setting in.pic_w = N*width with block_w = width and block_h = height/N, and multiply scale_y by N. on_trans_done is invoked for PPA_TRANS_MODE_BLOCKING calls as well, so a semaphore given by it is already set after any blocking call; take it with timeout 0 before switching to non-blocking, or the first wait returns early and the next submit fails with 'exceed maximum pending transactions' (pool size defaults to 1).
