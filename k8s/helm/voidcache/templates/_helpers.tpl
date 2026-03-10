{{/*
Expand the name of the chart.
*/}}
{{- define "voidcache.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Full release name, capped at 63 characters.
*/}}
{{- define "voidcache.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Chart label: name-version
*/}}
{{- define "voidcache.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels applied to all resources.
*/}}
{{- define "voidcache.labels" -}}
helm.sh/chart: {{ include "voidcache.chart" . }}
{{ include "voidcache.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels (used in matchLabels — must be stable, never change after install).
*/}}
{{- define "voidcache.selectorLabels" -}}
app.kubernetes.io/name: {{ include "voidcache.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Namespace to deploy into (supports namespaceOverride).
*/}}
{{- define "voidcache.namespace" -}}
{{- if .Values.namespaceOverride }}
{{- .Values.namespaceOverride }}
{{- else }}
{{- .Release.Namespace }}
{{- end }}
{{- end }}

{{/*
Image reference, including optional digest pin.
*/}}
{{- define "voidcache.image" -}}
{{- printf "%s:%s" .Values.image.repository .Values.image.tag }}
{{- end }}

{{/*
Headless service name for stable pod DNS.
*/}}
{{- define "voidcache.headlessService" -}}
{{- printf "%s-headless" (include "voidcache.fullname" .) }}
{{- end }}

{{/*
Pod DNS name for a given ordinal (used in ANNOUNCE_ADDR env var).
Usage: {{ include "voidcache.podDNS" (dict "ordinal" 0 "context" .) }}
*/}}
{{- define "voidcache.podDNS" -}}
{{- printf "%s-%d.%s.%s.svc.cluster.local"
    (include "voidcache.fullname" .context)
    (.ordinal | int)
    (include "voidcache.headlessService" .context)
    (include "voidcache.namespace" .context) }}
{{- end }}
