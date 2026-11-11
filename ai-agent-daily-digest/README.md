# X 上 AI Agent 每日热点博客

每日 UTC 01:00 自动抓取 X（Twitter）上过去 24 小时与 **AI Agent / 智能体** 相关的热点长帖、产品发布与博客，按互动热度排出 Top 10。

## 最新一期

- [2026-11-11](./2026-11-11.md)（窗口：2026-11-10 01:00 UTC → 2026-11-11 01:03 UTC）
- 机器可读：[2026-11-11.json](./2026-11-11.json)

## 排名方法

热度分 = `likes + 2×bookmarks + 3×reposts + 2×quotes + impressions/1000`

候选来自：

1. X News 中与 AI agent / agentic / 智能体 相关的故事簇
2. 过去 24 小时高互动原创帖（`-is:retweet`，带 `public_metrics`）
3. 被转发的外部博客 / arXiv / 产品说明

排除邀请码、NFT mint、纯广告引流。同一事件只保留一条代表帖。
