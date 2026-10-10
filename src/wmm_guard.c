/* wmm_guard.c —— guard/quarantine + 泄漏三档(M14,见 doc/tasks.md §16)
 *
 * 手写起点。默认关闭,运行期开关。poison/redzone、双释放/野指针事件、
 * C/B/A 三档泄漏检测(与 wmm_prof.c 联动)。
 */
