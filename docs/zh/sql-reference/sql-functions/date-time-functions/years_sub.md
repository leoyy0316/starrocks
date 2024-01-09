---
displayed_sidebar: "Chinese"
---

# years_sub

## 功能

从指定日期时间或日期减去指定年数。

## 语法

```Haskell
DATETIME YEARS_SUB(DATETIME date, INT years)
```

## 参数说明

`date`: 原始日期，类型为 DATETIME 或者 DATE。

`years`: 需要减去的年数。该值可以为负数，但需满足 `date` 的年份减去 `years` 不能超过 10000。假如 `date` 中的年份为 2022，那么 `years` 不能小于 -7979。同时，该值不能超过 `date` 中的年份，假如 `date` 中的年份为 2022，那么 `years` 不能大于 2022。

## 返回值说明

返回值与参数 `date` 类型一致。如果计算出的结果年份超出范围[0, 9999]，则返回 NULL。

## 示例

```Plain Text
select years_sub("2022-12-20 15:50:21", 2);
+-------------------------------------+
| years_sub('2022-12-20 15:50:21', 2) |
+-------------------------------------+
| 2020-12-20 15:50:21                 |
+-------------------------------------+
```