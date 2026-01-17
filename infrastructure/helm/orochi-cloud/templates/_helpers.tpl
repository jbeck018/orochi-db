{{/*
Expand the name of the chart.
*/}}
{{- define "orochi-cloud.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
We truncate at 63 chars because some Kubernetes name fields are limited to this (by the DNS naming spec).
If release name contains chart name it will be used as a full name.
*/}}
{{- define "orochi-cloud.fullname" -}}
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
Create chart name and version as used by the chart label.
*/}}
{{- define "orochi-cloud.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels
*/}}
{{- define "orochi-cloud.labels" -}}
helm.sh/chart: {{ include "orochi-cloud.chart" . }}
{{ include "orochi-cloud.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: orochi-cloud
{{- end }}

{{/*
Selector labels
*/}}
{{- define "orochi-cloud.selectorLabels" -}}
app.kubernetes.io/name: {{ include "orochi-cloud.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Namespace to use
*/}}
{{- define "orochi-cloud.namespace" -}}
{{- if .Values.namespace.create }}
{{- .Values.namespace.name | default "orochi-cloud" }}
{{- else }}
{{- .Release.Namespace }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Control Plane helpers
    ========================================================================== */}}

{{/*
Control Plane full name
*/}}
{{- define "orochi-cloud.controlPlane.fullname" -}}
{{- printf "%s-%s" (include "orochi-cloud.fullname" .) .Values.controlPlane.name | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Control Plane labels
*/}}
{{- define "orochi-cloud.controlPlane.labels" -}}
{{ include "orochi-cloud.labels" . }}
app.kubernetes.io/component: {{ .Values.controlPlane.name }}
{{- end }}

{{/*
Control Plane selector labels
*/}}
{{- define "orochi-cloud.controlPlane.selectorLabels" -}}
{{ include "orochi-cloud.selectorLabels" . }}
app.kubernetes.io/component: {{ .Values.controlPlane.name }}
{{- end }}

{{/*
Control Plane service account name
*/}}
{{- define "orochi-cloud.controlPlane.serviceAccountName" -}}
{{- if .Values.controlPlane.serviceAccount.create }}
{{- default (include "orochi-cloud.controlPlane.fullname" .) .Values.controlPlane.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.controlPlane.serviceAccount.name }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Dashboard helpers
    ========================================================================== */}}

{{/*
Dashboard full name
*/}}
{{- define "orochi-cloud.dashboard.fullname" -}}
{{- printf "%s-%s" (include "orochi-cloud.fullname" .) .Values.dashboard.name | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Dashboard labels
*/}}
{{- define "orochi-cloud.dashboard.labels" -}}
{{ include "orochi-cloud.labels" . }}
app.kubernetes.io/component: {{ .Values.dashboard.name }}
{{- end }}

{{/*
Dashboard selector labels
*/}}
{{- define "orochi-cloud.dashboard.selectorLabels" -}}
{{ include "orochi-cloud.selectorLabels" . }}
app.kubernetes.io/component: {{ .Values.dashboard.name }}
{{- end }}

{{/*
Dashboard service account name
*/}}
{{- define "orochi-cloud.dashboard.serviceAccountName" -}}
{{- if .Values.dashboard.serviceAccount.create }}
{{- default (include "orochi-cloud.dashboard.fullname" .) .Values.dashboard.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.dashboard.serviceAccount.name }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Autoscaler helpers
    ========================================================================== */}}

{{/*
Autoscaler full name
*/}}
{{- define "orochi-cloud.autoscaler.fullname" -}}
{{- printf "%s-%s" (include "orochi-cloud.fullname" .) .Values.autoscaler.name | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Autoscaler labels
*/}}
{{- define "orochi-cloud.autoscaler.labels" -}}
{{ include "orochi-cloud.labels" . }}
app.kubernetes.io/component: {{ .Values.autoscaler.name }}
{{- end }}

{{/*
Autoscaler selector labels
*/}}
{{- define "orochi-cloud.autoscaler.selectorLabels" -}}
{{ include "orochi-cloud.selectorLabels" . }}
app.kubernetes.io/component: {{ .Values.autoscaler.name }}
{{- end }}

{{/*
Autoscaler service account name
*/}}
{{- define "orochi-cloud.autoscaler.serviceAccountName" -}}
{{- if .Values.autoscaler.serviceAccount.create }}
{{- default (include "orochi-cloud.autoscaler.fullname" .) .Values.autoscaler.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.autoscaler.serviceAccount.name }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Provisioner helpers
    ========================================================================== */}}

{{/*
Provisioner full name
*/}}
{{- define "orochi-cloud.provisioner.fullname" -}}
{{- printf "%s-%s" (include "orochi-cloud.fullname" .) .Values.provisioner.name | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Provisioner labels
*/}}
{{- define "orochi-cloud.provisioner.labels" -}}
{{ include "orochi-cloud.labels" . }}
app.kubernetes.io/component: {{ .Values.provisioner.name }}
{{- end }}

{{/*
Provisioner selector labels
*/}}
{{- define "orochi-cloud.provisioner.selectorLabels" -}}
{{ include "orochi-cloud.selectorLabels" . }}
app.kubernetes.io/component: {{ .Values.provisioner.name }}
{{- end }}

{{/*
Provisioner service account name
*/}}
{{- define "orochi-cloud.provisioner.serviceAccountName" -}}
{{- if .Values.provisioner.serviceAccount.create }}
{{- default (include "orochi-cloud.provisioner.fullname" .) .Values.provisioner.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.provisioner.serviceAccount.name }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Image helpers
    ========================================================================== */}}

{{/*
Return the proper image name
*/}}
{{- define "orochi-cloud.image" -}}
{{- $registryName := .imageRoot.registry -}}
{{- $repositoryName := .imageRoot.repository -}}
{{- $tag := .imageRoot.tag | default .chart.AppVersion -}}
{{- if $registryName }}
{{- printf "%s/%s:%s" $registryName $repositoryName $tag -}}
{{- else }}
{{- printf "%s:%s" $repositoryName $tag -}}
{{- end }}
{{- end }}

{{/*
Return the proper image pull secrets
*/}}
{{- define "orochi-cloud.imagePullSecrets" -}}
{{- if .Values.global.imagePullSecrets }}
imagePullSecrets:
{{- range .Values.global.imagePullSecrets }}
  - name: {{ .name }}
{{- end }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Security Context helpers
    ========================================================================== */}}

{{/*
Return pod security context
*/}}
{{- define "orochi-cloud.podSecurityContext" -}}
{{- if .securityContext }}
securityContext:
  {{- toYaml .securityContext | nindent 2 }}
{{- end }}
{{- end }}

{{/*
Return container security context
*/}}
{{- define "orochi-cloud.containerSecurityContext" -}}
{{- if .securityContext }}
securityContext:
  {{- toYaml .securityContext | nindent 2 }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Resource helpers
    ========================================================================== */}}

{{/*
Return resource limits and requests
*/}}
{{- define "orochi-cloud.resources" -}}
{{- if .resources }}
resources:
  {{- toYaml .resources | nindent 2 }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Probe helpers
    ========================================================================== */}}

{{/*
Return liveness probe configuration
*/}}
{{- define "orochi-cloud.livenessProbe" -}}
{{- if .livenessProbe }}
livenessProbe:
  {{- toYaml .livenessProbe | nindent 2 }}
{{- end }}
{{- end }}

{{/*
Return readiness probe configuration
*/}}
{{- define "orochi-cloud.readinessProbe" -}}
{{- if .readinessProbe }}
readinessProbe:
  {{- toYaml .readinessProbe | nindent 2 }}
{{- end }}
{{- end }}

{{/* ==========================================================================
    Checksum helpers for secret/configmap changes triggering rolling updates
    ========================================================================== */}}

{{/*
Calculate checksum of configmap data
*/}}
{{- define "orochi-cloud.configmapChecksum" -}}
{{- $data := .data | toYaml -}}
{{ $data | sha256sum }}
{{- end }}

{{/*
Calculate checksum of secret data
*/}}
{{- define "orochi-cloud.secretChecksum" -}}
{{- $data := .data | toYaml -}}
{{ $data | sha256sum }}
{{- end }}
