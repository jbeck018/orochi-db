{{/*
Expand the name of the chart.
*/}}
{{- define "pgdog-router.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
We truncate at 63 chars because some Kubernetes name fields are limited to this (by the DNS naming spec).
If release name contains chart name it will be used as a full name.
*/}}
{{- define "pgdog-router.fullname" -}}
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
{{- define "pgdog-router.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels
*/}}
{{- define "pgdog-router.labels" -}}
helm.sh/chart: {{ include "pgdog-router.chart" . }}
{{ include "pgdog-router.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: orochi-cloud
{{- end }}

{{/*
Selector labels
*/}}
{{- define "pgdog-router.selectorLabels" -}}
app.kubernetes.io/name: {{ include "pgdog-router.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/component: connection-pooler
{{- end }}

{{/*
Create the name of the service account to use
*/}}
{{- define "pgdog-router.serviceAccountName" -}}
{{- if .Values.serviceAccount.create }}
{{- default (include "pgdog-router.fullname" .) .Values.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.serviceAccount.name }}
{{- end }}
{{- end }}

{{/*
Return the proper image name
*/}}
{{- define "pgdog-router.image" -}}
{{- $registryName := .Values.image.registry | default "" -}}
{{- $repositoryName := .Values.image.repository -}}
{{- $tag := .Values.image.tag | default .Chart.AppVersion -}}
{{- if $registryName }}
{{- printf "%s/%s:%s" $registryName $repositoryName $tag -}}
{{- else }}
{{- printf "%s:%s" $repositoryName $tag -}}
{{- end }}
{{- end }}

{{/*
Return the proper JWT proxy image name
*/}}
{{- define "pgdog-router.jwtProxyImage" -}}
{{- $repositoryName := .Values.jwtProxy.image.repository -}}
{{- $tag := .Values.jwtProxy.image.tag | default "latest" -}}
{{- printf "%s:%s" $repositoryName $tag -}}
{{- end }}

{{/*
Create the name of the config secret
*/}}
{{- define "pgdog-router.secretName" -}}
{{- if .Values.secrets.existingSecret }}
{{- .Values.secrets.existingSecret }}
{{- else }}
{{- include "pgdog-router.fullname" . }}
{{- end }}
{{- end }}

{{/*
Create the name of the TLS secret
*/}}
{{- define "pgdog-router.tlsSecretName" -}}
{{- if .Values.pgdog.tls.existingSecret }}
{{- .Values.pgdog.tls.existingSecret }}
{{- else }}
{{- printf "%s-tls" (include "pgdog-router.fullname" .) }}
{{- end }}
{{- end }}

{{/*
Create the name of the JWT secret
*/}}
{{- define "pgdog-router.jwtSecretName" -}}
{{- if .Values.jwtProxy.existingSecret }}
{{- .Values.jwtProxy.existingSecret }}
{{- else }}
{{- printf "%s-jwt" (include "pgdog-router.fullname" .) }}
{{- end }}
{{- end }}

{{/*
Return the appropriate apiVersion for HPA
*/}}
{{- define "pgdog-router.hpa.apiVersion" -}}
{{- if .Capabilities.APIVersions.Has "autoscaling/v2" }}
{{- print "autoscaling/v2" }}
{{- else }}
{{- print "autoscaling/v2beta2" }}
{{- end }}
{{- end }}

{{/*
Return the appropriate apiVersion for PDB
*/}}
{{- define "pgdog-router.pdb.apiVersion" -}}
{{- if .Capabilities.APIVersions.Has "policy/v1" }}
{{- print "policy/v1" }}
{{- else }}
{{- print "policy/v1beta1" }}
{{- end }}
{{- end }}

{{/*
Return the appropriate apiVersion for Ingress
*/}}
{{- define "pgdog-router.ingress.apiVersion" -}}
{{- if .Capabilities.APIVersions.Has "networking.k8s.io/v1" }}
{{- print "networking.k8s.io/v1" }}
{{- else if .Capabilities.APIVersions.Has "networking.k8s.io/v1beta1" }}
{{- print "networking.k8s.io/v1beta1" }}
{{- else }}
{{- print "extensions/v1beta1" }}
{{- end }}
{{- end }}

{{/*
Generate upstream servers configuration
*/}}
{{- define "pgdog-router.upstreamsConfig" -}}
{{- if .Values.pgdog.upstreams }}
[[pools]]
name = "default"
{{- range .Values.pgdog.upstreams }}

[[pools.shards]]
[[pools.shards.servers]]
host = "{{ .host }}"
port = {{ .port | default 5432 }}
role = "{{ .role | default "primary" }}"
{{- if .weight }}
weight = {{ .weight }}
{{- end }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Generate users configuration
*/}}
{{- define "pgdog-router.usersConfig" -}}
{{- if .Values.pgdog.users }}
{{- range .Values.pgdog.users }}
[[users]]
name = "{{ .name }}"
{{- if .databases }}
databases = [{{ range $i, $db := .databases }}{{ if $i }}, {{ end }}"{{ $db }}"{{ end }}]
{{- end }}
{{- if .poolSize }}
pool_size = {{ .poolSize }}
{{- end }}
{{- if .poolMode }}
pool_mode = "{{ .poolMode }}"
{{- end }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Checksum for config to trigger rolling update
*/}}
{{- define "pgdog-router.configChecksum" -}}
{{- include "pgdog-router.upstreamsConfig" . | sha256sum }}
{{- end }}

{{/*
Pod labels including custom labels
*/}}
{{- define "pgdog-router.podLabels" -}}
{{ include "pgdog-router.selectorLabels" . }}
{{- if .Values.podLabels }}
{{ toYaml .Values.podLabels }}
{{- end }}
{{- end }}
