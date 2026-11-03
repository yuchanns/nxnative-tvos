const archiveLimit = 64 * 1024 * 1024;
const shaPattern = /^[a-f0-9]{64}$/;

type Save = {
  version: 1;
  revision: string;
  sha256: string;
  bytes: number;
  savedAt: string;
  parent: string | null;
  commit: string;
};

function json(value: unknown, status = 200): Response {
  return Response.json(value, {
    status,
    headers: { "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff" },
  });
}

function isSave(value: unknown): value is Save {
  if (value === null || typeof value !== "object") return false;
  const save = value as Partial<Save>;
  return save.version === 1 && typeof save.revision === "string" &&
    shaPattern.test(save.revision) && save.sha256 === save.revision &&
    typeof save.commit === "string" && typeof save.bytes === "number" && Number.isSafeInteger(save.bytes) &&
    save.bytes >= 12 && save.bytes <= archiveLimit && typeof save.savedAt === "string" &&
    (save.parent === null || typeof save.parent === "string" && shaPattern.test(save.parent));
}

async function authorized(request: Request, token: string): Promise<boolean> {
  const header = request.headers.get("Authorization") ?? "";
  if (!/^[a-f0-9]{64}$/.test(token) || header.length > 256) return false;
  const encoder = new TextEncoder();
  const [actual, expected] = await Promise.all([
    crypto.subtle.digest("SHA-256", encoder.encode(header)),
    crypto.subtle.digest("SHA-256", encoder.encode(`Bearer ${token}`)),
  ]);
  return crypto.subtle.timingSafeEqual(actual, expected);
}

async function latest(bucket: R2Bucket, key: string): Promise<{ save: Save; etag: string } | null> {
  const object = await bucket.get(key);
  if (!object) return null;
  if (object.size > 2048) throw new Error("Invalid head size");
  const save: unknown = await object.json();
  if (!isSave(save)) throw new Error("Invalid head manifest");
  return { save, etag: object.etag };
}

async function handle(request: Request, env: Env): Promise<Response> {
  if (!await authorized(request, env.SAVE_TOKEN)) return json({ error: "unauthorized" }, 401);
  if (!/^[a-z0-9_-]{1,64}$/.test(env.SAVE_SCOPE)) throw new Error("Invalid save scope");
  const path = new URL(request.url).pathname;
  if (path === "/v1/info" && request.method === "GET") return json({ version: 1, scope: env.SAVE_SCOPE });
  const prefix = `saves/${env.SAVE_SCOPE}/`;
  const headKey = `${prefix}latest.json`;
  if (path === "/v1/latest" && request.method === "GET") {
    const head = await latest(env.SAVES, headKey);
    return head ? json(head.save) : json({ error: "no_save" }, 404);
  }
  const match = /^\/v1\/snapshots\/([a-f0-9]{64})$/.exec(path);
  if (!match) return json({ error: "not_found" }, 404);
  const sha = match[1];
  const objectKey = `${prefix}snapshots/${sha}.nxsave`;
  if (request.method === "GET") {
    const object = await env.SAVES.get(objectKey);
    if (!object) return json({ error: "not_found" }, 404);
    return new Response(object.body, { headers: {
      "Content-Type": "application/octet-stream",
      "Content-Length": String(object.size),
      "Cache-Control": "no-store",
      "X-Save-SHA256": sha,
      "ETag": object.httpEtag,
      "X-Content-Type-Options": "nosniff",
    } });
  }
  if (request.method !== "PUT") return json({ error: "method_not_allowed" }, 405);
  const length = request.headers.get("Content-Length") ?? "";
  const bytes = Number(length);
  const parent = request.headers.get("X-Save-Parent");
  if (!/^\d+$/.test(length) || !Number.isSafeInteger(bytes) || bytes < 12 || bytes > archiveLimit || !request.body)
    return json({ error: "invalid_archive_length" }, 400);
  if (parent !== "none" && (!parent || !shaPattern.test(parent))) return json({ error: "parent_required" }, 428);
  const head = await latest(env.SAVES, headKey);
  // A lost response is safe to retry without creating another version.
  if (head?.save.revision === sha) {
    if (head.save.bytes !== bytes) return json({ error: "invalid_archive_length" }, 400);
    return json(head.save);
  }
  if ((head?.save.revision ?? "none") !== parent) return json({ error: "conflict", latest: head?.save ?? null }, 409);

  const existing = await env.SAVES.head(objectKey);
  if (existing) {
    const checksum = existing.checksums.sha256;
    const digest = checksum ? Array.from(new Uint8Array(checksum), byte => byte.toString(16).padStart(2, "0")).join("") : "";
    if (existing.size !== bytes || digest !== sha) throw new Error("Snapshot integrity mismatch");
  }
  if (!existing) {
    // Content addressing and R2's checksum validation make concurrent writes of
    // this key equivalent. The head remains unchanged until the body is durable.
    const stream = new FixedLengthStream(bytes);
    const abort = new AbortController();
    try {
      await Promise.all([
        env.SAVES.put(objectKey, stream.readable, {
          sha256: sha,
          httpMetadata: { contentType: "application/octet-stream" },
          customMetadata: { sha256: sha },
        }),
        request.body.pipeTo(stream.writable, { signal: abort.signal }),
      ]);
    } finally {
      abort.abort();
    }
  }
  const save: Save = { version: 1, revision: sha, sha256: sha, bytes,
    savedAt: new Date().toISOString(), parent: parent === "none" ? null : parent, commit: crypto.randomUUID() };
  const published = await env.SAVES.put(headKey, JSON.stringify(save), {
    onlyIf: head ? { etagMatches: head.etag } : { etagDoesNotMatch: "*" },
    httpMetadata: { contentType: "application/json", cacheControl: "no-store" },
  });
  if (!published) {
    const current = await latest(env.SAVES, headKey);
    if (current?.save.revision === sha) return json(current.save);
    return json({ error: "conflict", latest: current?.save ?? null }, 409);
  }
  console.log(JSON.stringify({ event: "save_published", scope: env.SAVE_SCOPE, bytes, revision: sha }));
  return json(save, 201);
}

export default {
  async fetch(request, env): Promise<Response> {
    try { return await handle(request, env); }
    catch {
      console.error(JSON.stringify({ event: "save_request_failed", method: request.method }));
      return json({ error: "storage_unavailable" }, 503);
    }
  },
} satisfies ExportedHandler<Env>;
