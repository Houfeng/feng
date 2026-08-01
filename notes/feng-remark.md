# 备注/备忘

- 所有加了 !! 后缀的 commit，都是将来需要再斟酌或优化的 commit


在 feng 中

test(string, int...) 和 test(string, int[]), 不冲突，无调用歧义

test(string, int...) 和 test(string, int), 冲突，有调用歧义，且无法消歧义