# 测试

每个模块对应一个 `test_*.c`,自测清单见 `doc/tasks.md` 各模块小节。

| 文件 | 覆盖(tasks.md 模块) | SPEC |
|---|---|---|
| test_sc.c | M1 尺寸类/查表 | SC1/SC2/SC3 |
| test_slab.c | M3 slab + freelist + bin | SLAB1/2, AR1 |
| test_pte.c | M4 页表登记/反查 | PTE1/2 |
| test_api.c | M4/M5 API 契约 | API1 |
| test_oracle.c | M7 oracle 属性测试 | G1/G2, SC2, PTE2 |
| test_stress.c | M7 多线程压力 | TC1, G2/G5, PHY1 |
| test_tcache.c | M8/M9 快路径/GC/线程退出 | TC1~TC4, G5 |
| test_phys.c | M6/M10 合并/purge | PHY1/2/3 |
| test_stats.c | M11 计数/事件 | OBS1 |
| test_shm.c | M12 共享内存 | SHM1 |
| test_prof.c | M13 采样 | OBS2 |
| test_guard.c | M14 guard/泄漏三档 | OBS3/4 |
| test_wrap.c | M15 --wrap 集成 | G3 |

统一入口:写一个 `test_main` 表(每个 test 文件一个函数),由 `tests/main.c` 汇总,
`make test` 全部执行。
