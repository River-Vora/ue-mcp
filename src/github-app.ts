import { createSign } from "node:crypto";
import { resolveUserAuth, clearUserAuth, type PendingDeviceFlow } from "./auth.js";
import { loadAppManifestSignature } from "./manifest-signature.js";
import { CORE_REPO, sameRepo, type GitHubRepo } from "./registry-catalog.js";

const APP_ID = "3133514";

/**
 * A report can be filed against a plugin's own tracker instead of core - see
 * src/feedback-routing.ts. Everything below takes the target repo as a
 * parameter and defaults to core, so existing callers are unaffected.
 */
function issuesEndpoint(repo: GitHubRepo): string {
  return `https://api.github.com/repos/${repo.owner}/${repo.repo}/issues`;
}

/**
 * Statuses that mean "this tracker will not take the issue" rather than
 * "something went wrong". Only meaningful off-core: the core tracker is known
 * good, so a failure there is a real error worth throwing.
 *
 *   403 - the token cannot open issues here
 *   404 - repo missing, renamed, or private to this token
 *   410 - issues are disabled on the repo
 */
const REPO_REFUSED_STATUSES = new Set([403, 404, 410]);

function base64url(input: string): string {
  return Buffer.from(input).toString("base64url");
}

function createJWT(pem: string): string {
  const now = Math.floor(Date.now() / 1000);
  const header = base64url(JSON.stringify({ alg: "RS256", typ: "JWT" }));
  const payload = base64url(
    JSON.stringify({
      iss: APP_ID,
      iat: now - 60,
      exp: now + 600,
    }),
  );

  const unsigned = `${header}.${payload}`;
  const sign = createSign("RSA-SHA256");
  sign.update(unsigned);
  const signature = sign.sign(pem, "base64url");

  return `${unsigned}.${signature}`;
}

async function getInstallationToken(jwt: string): Promise<string> {
  const res = await fetch("https://api.github.com/app/installations", {
    headers: {
      Authorization: `Bearer ${jwt}`,
      Accept: "application/vnd.github+json",
    },
  });

  if (!res.ok) {
    throw new Error(`GitHub App auth failed: ${res.status}`);
  }

  const installations = (await res.json()) as Array<{ id: number }>;
  if (installations.length === 0) {
    throw new Error("GitHub App has no installations");
  }

  const tokenRes = await fetch(
    `https://api.github.com/app/installations/${installations[0].id}/access_tokens`,
    {
      method: "POST",
      headers: {
        Authorization: `Bearer ${jwt}`,
        Accept: "application/vnd.github+json",
      },
    },
  );

  if (!tokenRes.ok) {
    throw new Error(`Failed to get installation token: ${tokenRes.status}`);
  }

  return ((await tokenRes.json()) as { token: string }).token;
}

export type SubmitResult =
  | {
      kind: "submitted";
      url: string;
      number: number;
      authoredBy: string;
      authoredAs: "user" | "bot";
      repo: string;
    }
  | {
      kind: "auth_required";
      verification_uri: string;
      user_code: string;
      expires_in: number;
    }
  | {
      /** A plugin tracker refused the post. Core never returns this. */
      kind: "repo_unavailable";
      repo: string;
      status: number;
      message: string;
    };

async function submitAsBot(
  title: string,
  body: string,
  labels: string[],
  repo: GitHubRepo,
): Promise<SubmitResult> {
  // Anonymous bot path. The loader is lazy so the default useBot=false flow
  // never even reads the asset. Moving this to a server-side proxy is the
  // long-term plan — see https://github.com/db-lyon/ue-mcp/issues/461.
  const jwt = createJWT(loadAppManifestSignature());
  const token = await getInstallationToken(jwt);
  const res = await fetch(issuesEndpoint(repo), {
    method: "POST",
    headers: {
      Authorization: `token ${token}`,
      Accept: "application/vnd.github+json",
      "Content-Type": "application/json",
      "User-Agent": "ue-mcp",
    },
    body: JSON.stringify({ title, body, labels }),
  });
  if (!res.ok) {
    const text = await res.text();
    // The GitHub App is installed on the core tracker. A plugin repo it was
    // never installed on refuses the token, and that is a routing outcome the
    // caller can recover from (re-file as the user, or hand over a prefilled
    // URL) - not a crash.
    if (!sameRepo(repo, CORE_REPO) && REPO_REFUSED_STATUSES.has(res.status)) {
      return { kind: "repo_unavailable", repo: `${repo.owner}/${repo.repo}`, status: res.status, message: text.slice(0, 300) };
    }
    throw new Error(`Failed to create issue (bot): ${res.status} ${text}`);
  }
  const issue = (await res.json()) as { html_url: string; number: number };
  return {
    kind: "submitted",
    url: issue.html_url,
    number: issue.number,
    authoredBy: "ue-mcp-feedback[bot]",
    authoredAs: "bot",
    repo: `${repo.owner}/${repo.repo}`,
  };
}

function pendingResult(pending: PendingDeviceFlow): SubmitResult {
  return {
    kind: "auth_required",
    verification_uri: pending.verification_uri,
    user_code: pending.user_code,
    expires_in: Math.max(0, pending.expires_at - Math.floor(Date.now() / 1000)),
  };
}

export async function submitFeedback(
  title: string,
  body: string,
  labels: string[] = ["agent-feedback"],
  options: { useBot?: boolean; repo?: GitHubRepo } = {},
): Promise<SubmitResult> {
  const repo = options.repo ?? CORE_REPO;
  const repoName = `${repo.owner}/${repo.repo}`;

  if (options.useBot) {
    return submitAsBot(title, body, labels, repo);
  }

  const auth = await resolveUserAuth();
  if (auth.kind === "pending") {
    return pendingResult(auth.pending);
  }

  const post = (token: string) =>
    fetch(issuesEndpoint(repo), {
      method: "POST",
      headers: {
        Authorization: `token ${token}`,
        Accept: "application/vnd.github+json",
        "Content-Type": "application/json",
        "User-Agent": "ue-mcp",
      },
      body: JSON.stringify({ title, body, labels }),
    });

  const res = await post(auth.auth.token);

  if (res.status === 401) {
    // Token revoked or expired. Wipe and re-initiate device flow on the next
    // call so the user gets a fresh code instead of a silent bot fallback.
    await clearUserAuth();
    const retry = await resolveUserAuth();
    if (retry.kind === "pending") return pendingResult(retry.pending);
    // Fresh auth landed somehow - fall through to retry the post.
    const res2 = await post(retry.auth.token);
    if (!res2.ok) {
      const text = await res2.text();
      if (!sameRepo(repo, CORE_REPO) && REPO_REFUSED_STATUSES.has(res2.status)) {
        return { kind: "repo_unavailable", repo: repoName, status: res2.status, message: text.slice(0, 300) };
      }
      throw new Error(`Failed to create issue as user (after re-auth): ${res2.status} ${text}`);
    }
    const issue2 = (await res2.json()) as { html_url: string; number: number };
    return {
      kind: "submitted",
      url: issue2.html_url,
      number: issue2.number,
      authoredBy: retry.auth.login,
      authoredAs: "user",
      repo: repoName,
    };
  }

  if (!res.ok) {
    const text = await res.text();
    if (!sameRepo(repo, CORE_REPO) && REPO_REFUSED_STATUSES.has(res.status)) {
      return { kind: "repo_unavailable", repo: repoName, status: res.status, message: text.slice(0, 300) };
    }
    throw new Error(`Failed to create issue as user: ${res.status} ${text}`);
  }

  const issue = (await res.json()) as { html_url: string; number: number };
  return {
    kind: "submitted",
    url: issue.html_url,
    number: issue.number,
    authoredBy: auth.auth.login,
    authoredAs: "user",
    repo: repoName,
  };
}
