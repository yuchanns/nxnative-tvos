# R2 自动存档

这个 Worker 只负责一个游戏的私有存档空间。tvOS 在成功提交本地事务时创建 APFS 快照及文件清单，后台打包上传；首次启动、再次启动和缓存被清理后，都在挂载游戏存档前核对云端。

## 一致性与恢复

- `NXSAVE01` 包包含受限的 JSON 清单、空目录、文件内容、每文件 SHA-256 和文件名别名属性。完整包上限 64 MiB，清单上限 1 MiB，最多 4096 项。
- `snapshots/<sha256>.nxsave` 按内容寻址。Worker 通过 R2 校验 SHA-256，写入成功后用 ETag 条件更新 `latest.json`；上传中断不会发布半份存档。
- 请求携带 `X-Save-Parent`。云端父版本不符时返回 409，客户端保留本地待上传版本。响应丢失可幂等重试。
- 打包途中退出后从完整 capture 恢复；启动时仅接受清单完整的 capture 或整包校验通过的 pending。恢复先解包到新目录，再原子交换到游戏存档目录。
- 非空本地存档如果与可验证快照不一致，会暂停启动并保留两份数据，避免把一次捕获失败后的新进度自动回滚。完整缓存清理后，本地为空时自动恢复云端。
- 断网时可从已校验的本地包继续游戏。上传未确认的进度仍可能随 tvOS 清理缓存而丢失；界面会持续显示待同步，不能将它称为已经保存在云端。
- 当前保留全部不同内容的云端版本，重复内容不重复占用空间。不要给整个存档前缀添加到期删除规则，否则可能删除仍被 `latest.json` 引用的版本。自动历史清理尚未实现。

## 部署

先完成 Cloudflare 授权，检查现有桶和 Worker 名称，避免覆盖无关服务。桶保持私有，公开访问由带认证的 Worker 提供。

```sh
npm ci
npx wrangler whoami
npx wrangler r2 bucket list
npx wrangler r2 bucket create nxnative-saves
npx wrangler secret put SAVE_TOKEN --env "" < /private/path/cloud-save-token.txt
npx wrangler deploy --env ""
npx wrangler types
npm run check
```

`SAVE_TOKEN` 是随机生成的 32 字节、64 位十六进制设备令牌，不是 Cloudflare 账户管理令牌。Worker 绑定的 `SAVE_SCOPE` 决定可访问的存档前缀，设备不能切换空间。

IPA 的环境文件只配置公开地址：

```text
DM_CLOUD_SAVE_URL=https://your-save-worker.example.workers.dev
```

首次安装后，将 `DM_CLOUD_SAVE_TOKEN` 通过设备私有配置写入一次。应用将其存入本机钥匙串，成功后移除缓存环境文件里的这一项。构建脚本拒绝将该令牌打包进可分发 IPA。钥匙串不依赖 Caches，也不需要 CloudKit 权限；更换签名团队或应用标识可能需要重新配置。

## 验证

`verification` 环境使用同一私有桶内独立的 `validation-cloud-save` 前缀和独立设备令牌。下面的检查只请求真实部署；脚本首先核对验证空间，拒绝操作游戏空间。

```sh
npx wrangler secret put SAVE_TOKEN --env verification < /private/path/verification-token.txt
npx wrangler deploy --env verification
python3 tools/verify_remote.py \
  --url https://your-verification-worker.example.workers.dev \
  --token-file /private/path/verification-token.txt \
  --report /private/path/remote-verification.json
```

远端检查覆盖认证、完整上传及下载、幂等重试、错误校验和、并发写入竞争、陈旧父版本拒绝和旧版本保留。tvOS 仍须实机核验钥匙串存取、真实存档触发上传、断线重试及缓存移除后的启动恢复。编译和类型检查不能代替这些实机验证。
