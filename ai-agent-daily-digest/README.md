# X 上 AI Agent 每日热点博客

每日 UTC 01:00 自动抓取 X（Twitter）上过去 24 小时与 **AI Agent / 智能体** 相关的热点长帖、产品发布与博客，排出 Top 10。

## 最新一期

- [2026-11-14](./2026-11-14.md)（窗口：2026-11-13 01:00 UTC → 2026-11-14 01:04 UTC）
- 机器可读：[2026-11-14.json](./2026-11-14.json)

## 排名方法

优先使用帖级互动热度分：

`likes + 2×bookmarks + 3×reposts + 2×quotes + impressions/1000`

若 X Posts 检索额度耗尽，则退化为：

1. X News 中与 AI agent / agentic / 智能体 相关的 24h 故事簇
2. 簇内独立帖数量（讨论广度）
3. `news.updated_at` 新近程度
4. 是否有可核对的原文博客 / 产品说明

候选来自：

1. X News 故事簇（`search_news query="AI agent"`，`max_age_hours=24`）
2. 过去 24 小时高互动原创帖（`-is:retweet`，带 `public_metrics`）
3. 被转发的外部博客 / 研究页 / 产品说明

排除邀请码、NFT mint、纯广告引流、与 Agent 无关的娱乐八卦。同一事件只保留一条代表帖。昨日已覆盖且无明显新进展的条目标为「持续发酵」，不重复占满 Top 10。
