# VoidCache — Kubernetes Guide

## Architecture

```
                    ┌────────────────────────────────────────────┐
  Your app          │            Kubernetes cluster               │
  redis-py          │                                             │
  ioredis           │  Service: vcache (ClusterIP/LoadBalancer)  │
       │            │          port 6379                          │
       └────────────►                                             │
                    │  Deployment: vcache-haproxy (×2)           │
                    │  ┌───────────────────────────────────────┐  │
                    │  │ HAProxy  leastconn + health-check     │  │
                    │  └──────┬──────────────┬──────────────┬──┘  │
                    │         │              │              │      │
                    │  StatefulSet: vcache (×3)             │      │
                    │  ┌──────▼──────┐ ┌────▼────┐ ┌───────▼──┐  │
                    │  │  vcache-0   │ │vcache-1 │ │vcache-2  │  │
                    │  │  :6379      │ │  :6379  │ │  :6379   │  │
                    │  │  PVC: 10Gi  │ │ PVC:10Gi│ │ PVC:10Gi │  │
                    │  └─────────────┘ └─────────┘ └──────────┘  │
                    │                                             │
                    │  Headless Service: vcache-headless          │
                    │  → vcache-{n}.vcache-headless.voidcache…   │
                    └────────────────────────────────────────────┘
```

---

## Quick Start

Pick your environment:

### minikube / kind (local)

```bash
# 1. Start a local cluster
minikube start --cpus=4 --memory=4096
# or: kind create cluster --name voidcache

# 2. Build the image directly into the cluster (no registry needed)
eval $(minikube docker-env)             # minikube
docker build -t voidcache:latest .
# kind: kind load docker-image voidcache:latest --name voidcache

# 3. Deploy dev overlay (1 replica, no WAL, NodePort)
kubectl apply -k k8s/overlays/dev

# 4. Wait for pods
kubectl rollout status statefulset/vcache -n voidcache --timeout=120s

# 5. Forward port and test
kubectl port-forward svc/vcache 6379:6379 -n voidcache &
redis-cli -h localhost -p 6379 PING   # → PONG
```

### Cloud cluster (EKS / GKE / AKS)

```bash
# 1. Build and push to your registry
docker build -t ghcr.io/YOUR_ORG/voidcache:2.0.0 .
docker push ghcr.io/YOUR_ORG/voidcache:2.0.0

# 2. Update the image in the prod overlay
sed -i 's|newName: .*|newName: ghcr.io/YOUR_ORG/voidcache|' k8s/overlays/prod/kustomization.yaml
sed -i 's|newTag: .*|newTag: "2.0.0"|' k8s/overlays/prod/kustomization.yaml

# 3. Deploy
kubectl apply -k k8s/overlays/prod

# 4. Get the LoadBalancer address
kubectl get svc vcache -n voidcache
```

### Helm

```bash
# 3-node cluster, ClusterIP service, no auth
helm install voidcache ./k8s/helm/voidcache \
  --namespace voidcache \
  --create-namespace

# With auth + LoadBalancer + production sizing
helm install voidcache ./k8s/helm/voidcache \
  --namespace voidcache \
  --create-namespace \
  --set image.repository=ghcr.io/YOUR_ORG/voidcache \
  --set image.tag=2.0.0 \
  --set auth.enabled=true \
  --set auth.password=supersecret \
  --set service.type=LoadBalancer \
  --set cache.maxMemory=1500m \
  --set cache.threads=8 \
  --set cache.persistence.storageClass=fast-ssd

# Upgrade
helm upgrade voidcache ./k8s/helm/voidcache --reuse-values --set image.tag=2.1.0

# Uninstall (keeps PVCs by default)
helm uninstall voidcache -n voidcache
```

---

## Kompose: docker-compose → Kubernetes

If you prefer to start from the docker-compose file and convert to raw manifests:

```bash
# Install kompose
brew install kompose                          # macOS
curl -L https://github.com/kubernetes/kompose/releases/latest/download/kompose-linux-amd64 \
  -o /usr/local/bin/kompose && chmod +x /usr/local/bin/kompose   # Linux

# Convert the annotated compose file to k8s manifests
kompose convert -f docker-compose.k8s.yml -o k8s/base/

# Review what was generated
ls k8s/base/

# Apply
kubectl apply -f k8s/base/
```

The `docker-compose.k8s.yml` file has Kompose annotations that produce:
- `StatefulSet` (not Deployment) for cache nodes via `kompose.controller.type: statefulset`
- `PersistentVolumeClaim` per node via `kompose.volume.size: 10Gi`
- `LoadBalancer` Service for HAProxy via `kompose.service.type: LoadBalancer`

---

## File Layout

```
VoidCache/
├── Dockerfile                       Multi-stage build
├── docker-compose.k8s.yml           Kompose-annotated compose (dual purpose)
│
├── k8s/
│   ├── base/                        Raw Kubernetes manifests
│   │   ├── 00-namespace.yaml
│   │   ├── 01-configmap.yaml        ACL file + HAProxy config
│   │   ├── 02-secret.yaml           VC_PASSWORD
│   │   ├── 03-services.yaml         Headless + ClusterIP + NodePort
│   │   ├── 04-statefulset.yaml      3-node StatefulSet with PVCs
│   │   ├── 05-haproxy.yaml          HAProxy Deployment (×2)
│   │   ├── 06-pdb-hpa.yaml          PodDisruptionBudget + HPA
│   │   ├── 07-networkpolicy.yaml    Traffic isolation
│   │   ├── 08-tls-cert.yaml         cert-manager Certificate
│   │   ├── 09-monitoring.yaml       ServiceMonitor + Grafana dashboard
│   │   └── kustomization.yaml
│   │
│   ├── overlays/
│   │   ├── dev/kustomization.yaml   1 replica, no WAL, NodePort, tiny resources
│   │   └── prod/kustomization.yaml  3 replicas, LoadBalancer, large PVCs, SSD
│   │
│   └── helm/voidcache/
│       ├── Chart.yaml
│       ├── values.yaml              All tunables with defaults
│       └── templates/
│           ├── _helpers.tpl
│           ├── statefulset.yaml
│           ├── services.yaml        Headless + client Service + HAProxy + ConfigMap
│           ├── secret.yaml
│           └── misc.yaml            PDB + HPA + NetworkPolicy + ServiceMonitor
│
└── .github/workflows/
    └── ci-cd.yml                    Build → test → push → deploy pipeline
```

---

## Deploying

### Raw manifests

```bash
# Apply everything
kubectl apply -k k8s/overlays/dev     # local dev
kubectl apply -k k8s/overlays/prod    # production

# Watch pods start up
kubectl get pods -n voidcache -w

# Check StatefulSet rollout
kubectl rollout status statefulset/vcache -n voidcache

# Check HAProxy
kubectl rollout status deployment/vcache-haproxy -n voidcache
```

### Verify the cluster

```bash
# PING all 3 cache nodes directly
for i in 0 1 2; do
  kubectl exec -n voidcache vcache-$i -- vcli -h 127.0.0.1 -p 6379 --no-color PING
done

# PING through HAProxy
kubectl run test --rm -it --image=voidcache:latest --restart=Never \
  --command -- vcli -h vcache.voidcache.svc.cluster.local -p 6379 PING

# Check VCINFO on each node
for i in 0 1 2; do
  echo "=== vcache-$i ===";
  kubectl exec -n voidcache vcache-$i -- vcli -h 127.0.0.1 -p 6379 --no-color VCINFO
done
```

---

## Configuration

### Change memory or thread count

```bash
# Kustomize: patch the StatefulSet in your overlay
kubectl patch statefulset vcache -n voidcache \
  --type=json \
  -p='[{"op":"replace","path":"/spec/template/spec/containers/0/env/0/value","value":"1g"}]'

# Helm
helm upgrade voidcache ./k8s/helm/voidcache \
  --reuse-values \
  --set cache.maxMemory=1g \
  --set cache.threads=8
```

### Enable authentication

```bash
# Create the secret (never put the password in YAML committed to git)
kubectl create secret generic vcache-secret \
  --namespace=voidcache \
  --from-literal=VC_PASSWORD=my_strong_password

# Restart pods to pick it up
kubectl rollout restart statefulset/vcache -n voidcache
```

---

## Rolling Update

StatefulSet updates pods highest-ordinal-first (vcache-2 → vcache-1 → vcache-0),
one at a time. HAProxy removes the pod from rotation during restart.

```bash
# Update image
kubectl set image statefulset/vcache vcache=ghcr.io/yourorg/voidcache:2.1.0 -n voidcache

# Watch the ordered rollout
kubectl rollout status statefulset/vcache -n voidcache --timeout=300s

# Canary: update only the highest ordinal first (partition=2 → only vcache-2 updates)
kubectl patch statefulset vcache -n voidcache \
  -p '{"spec":{"updateStrategy":{"rollingUpdate":{"partition":2}}}}'
# Verify vcache-2, then roll out the rest:
kubectl patch statefulset vcache -n voidcache \
  -p '{"spec":{"updateStrategy":{"rollingUpdate":{"partition":0}}}}'
```

---

## Scaling

Cache nodes use a StatefulSet — **do not** scale down without first migrating data,
as the evicted pod's WAL data becomes unavailable. HAProxy autoscales freely.

```bash
# Scale HAProxy up/down
kubectl scale deployment vcache-haproxy --replicas=4 -n voidcache

# Scale cache nodes UP (adds pods + PVCs; update haproxy ConfigMap to include new node)
kubectl scale statefulset vcache --replicas=5 -n voidcache

# After scaling cache up, reload HAProxy config:
kubectl rollout restart deployment/vcache-haproxy -n voidcache
```

---

## TLS with cert-manager

```bash
# Install cert-manager
kubectl apply -f https://github.com/cert-manager/cert-manager/releases/latest/download/cert-manager.yaml
kubectl wait --for=condition=Available deployment --all -n cert-manager --timeout=120s

# Enable the TLS resources in base/kustomization.yaml
# (uncomment the 08-tls-cert.yaml line, then apply)
kubectl apply -k k8s/overlays/prod

# cert-manager auto-generates and rotates the certificate.
# Verify
kubectl describe certificate vcache-tls -n voidcache
kubectl get secret vcache-tls-secret -n voidcache
```

---

## Monitoring

```bash
# Install kube-prometheus-stack
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts
helm install kube-prom prometheus-community/kube-prometheus-stack \
  --namespace monitoring --create-namespace

# Enable ServiceMonitor in your overlay (uncomment 09-monitoring.yaml line)
kubectl apply -k k8s/overlays/prod

# Access Grafana
kubectl port-forward svc/kube-prom-grafana 3000:80 -n monitoring
# Login: admin / prom-operator
# The VoidCache dashboard is auto-imported (grafana_dashboard: "1" label)
```

---

## CI/CD (GitHub Actions)

The pipeline at `.github/workflows/ci-cd.yml`:

1. **On every push** — compiles the binary, runs 25 unit tests, smoke-tests the server
2. **Builds Docker image** — multi-platform, tagged with branch/semver/SHA
3. **Pushes to `ghcr.io`** — skipped on PRs
4. **Deploys to dev** — on `develop` branch, Kustomize apply + rollout wait
5. **Deploys to prod** — on `v*.*.*` tag, requires manual approval in GitHub Environments

Setup:

```bash
# Add these secrets to your GitHub repo (Settings → Secrets → Actions):
# KUBE_CONFIG_DEV  — base64-encoded kubeconfig for your dev cluster
# KUBE_CONFIG_PROD — base64-encoded kubeconfig for your prod cluster

base64 -w0 ~/.kube/config-dev  | pbcopy   # copy to KUBE_CONFIG_DEV
base64 -w0 ~/.kube/config-prod | pbcopy   # copy to KUBE_CONFIG_PROD
```

---

## Troubleshooting

### Pods stuck in `Pending`

```bash
kubectl describe pod vcache-0 -n voidcache
# Common cause: no PersistentVolume available for the PVC
# Fix: ensure a StorageClass exists
kubectl get storageclass
# For local dev: kubectl apply -f https://raw.githubusercontent.com/rancher/local-path-provisioner/master/deploy/local-path-storage.yaml
```

### Pod CrashLoopBackOff

```bash
kubectl logs vcache-0 -n voidcache --previous
# Check: port conflicts, insufficient memory, bad WAL file
```

### HAProxy shows nodes as DOWN

```bash
# Port-forward the stats page
kubectl port-forward svc/vcache 8404:8404 -n voidcache
open http://localhost:8404/stats

# Check HAProxy logs
kubectl logs deployment/vcache-haproxy -n voidcache

# Verify cache pods are reachable from haproxy pod
kubectl exec -n voidcache deployment/vcache-haproxy -- \
  wget -qO- http://vcache-0.vcache-headless.voidcache.svc.cluster.local:6379 2>&1 || true
```

### DNS not resolving pod names

```bash
# Test from inside the cluster
kubectl run dns-test --rm -it --image=busybox --restart=Never -- \
  nslookup vcache-0.vcache-headless.voidcache.svc.cluster.local
# Should return an IP; if NXDOMAIN, the headless Service selector may not match
kubectl get endpoints vcache-headless -n voidcache
```
