## 1. Create the secrets file

```bash
# In PowerShell — run from the project root
# Leave VC_PASSWORD blank to use the ACL file instead
Copy-Item docker\.env docker\.env.local
```
The ACL file at `docker\config\vcache.acl` 
already has the correct credentials. 
You don't need to set VC_PASSWORD unless you want simple password-only mode.

## 2. Build and start

```bash
# Open PowerShell in the project root folder
# (right-click the folder → "Open in Terminal")

docker compose -f docker\docker-compose.yml up --build -d
```
First build takes 2–3 minutes (downloads Ubuntu base, compiles C source). Subsequent builds are ~20 seconds.

3. Check that all containers are healthy
```bash
docker compose -f docker\docker-compose.yml ps
```

Wait ~30 seconds after start. All four containers (vcache-1, vcache-2, vcache-3, vcache-haproxy) should show healthy or running.

Connect on localhost:6379 (HAProxy) — this is what StackExchange.Redis and RedisInsight should point at.
```
Port map
Port	What	Who connects here
6379	HAProxy — main entry point	Your app RedisInsight redis-cli
8404	HAProxy stats UI	Browser → http://localhost:8404/stats (user: admin / pass: admin)
6381	vcache-0 /
```