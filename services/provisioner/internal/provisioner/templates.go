// Package provisioner provides template rendering for Kubernetes resources.
package provisioner

import (
	"bytes"
	"embed"
	"fmt"
	"io/fs"
	"path/filepath"
	"text/template"

	"go.uber.org/zap"
	"gopkg.in/yaml.v3"

	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

//go:embed templates/*.yaml.tmpl
var templateFS embed.FS

// TemplateEngine renders Kubernetes resource templates
type TemplateEngine struct {
	templates *template.Template
	logger    *zap.Logger
}

// NewTemplateEngine creates a new template engine
func NewTemplateEngine(logger *zap.Logger) (*TemplateEngine, error) {
	// Parse all templates from embedded filesystem
	tmpl := template.New("").Funcs(templateFuncs())

	err := fs.WalkDir(templateFS, "templates", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		if filepath.Ext(path) != ".tmpl" {
			return nil
		}

		content, err := templateFS.ReadFile(path)
		if err != nil {
			return fmt.Errorf("failed to read template %s: %w", path, err)
		}

		name := filepath.Base(path)
		if _, err := tmpl.New(name).Parse(string(content)); err != nil {
			return fmt.Errorf("failed to parse template %s: %w", name, err)
		}

		return nil
	})

	if err != nil {
		// If no templates found, create default templates inline
		logger.Warn("no templates found in embedded filesystem, using defaults")
		tmpl, err = createDefaultTemplates()
		if err != nil {
			return nil, err
		}
	}

	return &TemplateEngine{
		templates: tmpl,
		logger:    logger,
	}, nil
}

// templateFuncs returns template helper functions
func templateFuncs() template.FuncMap {
	return template.FuncMap{
		"default": func(defaultValue, value interface{}) interface{} {
			if value == nil || value == "" {
				return defaultValue
			}
			return value
		},
		"toYaml": func(v interface{}) string {
			data, err := yaml.Marshal(v)
			if err != nil {
				return ""
			}
			return string(data)
		},
		"indent": func(spaces int, v string) string {
			pad := ""
			for i := 0; i < spaces; i++ {
				pad += " "
			}
			result := ""
			for i, line := range splitLines(v) {
				if i > 0 {
					result += "\n"
				}
				if line != "" {
					result += pad + line
				}
			}
			return result
		},
		"nindent": func(spaces int, v string) string {
			pad := ""
			for i := 0; i < spaces; i++ {
				pad += " "
			}
			result := "\n"
			for i, line := range splitLines(v) {
				if i > 0 {
					result += "\n"
				}
				if line != "" {
					result += pad + line
				}
			}
			return result
		},
		"quote": func(v string) string {
			return fmt.Sprintf("%q", v)
		},
		"boolToString": func(b bool) string {
			if b {
				return "true"
			}
			return "false"
		},
	}
}

func splitLines(s string) []string {
	var lines []string
	current := ""
	for _, c := range s {
		if c == '\n' {
			lines = append(lines, current)
			current = ""
		} else {
			current += string(c)
		}
	}
	if current != "" {
		lines = append(lines, current)
	}
	return lines
}

// createDefaultTemplates creates default templates if none are found
func createDefaultTemplates() (*template.Template, error) {
	tmpl := template.New("").Funcs(templateFuncs())

	// Cluster template
	clusterTemplate := `apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: {{ .Name }}
  namespace: {{ .Namespace }}
  labels:
    app.kubernetes.io/name: orochi-db
    app.kubernetes.io/instance: {{ .Name }}
    app.kubernetes.io/component: database
    app.kubernetes.io/managed-by: orochi-provisioner
    orochi.io/cluster-name: {{ .Name }}
{{- if .Labels }}
{{ toYaml .Labels | indent 4 }}
{{- end }}
{{- if .Annotations }}
  annotations:
{{ toYaml .Annotations | indent 4 }}
{{- end }}
spec:
  instances: {{ .Instances }}
  imageName: {{ default "ghcr.io/orochi-db/orochi-pg:16" .ImageName }}

  postgresql:
    parameters:
      shared_preload_libraries: "orochi"
{{- if .OrochiConfig }}
{{- if .OrochiConfig.DefaultShardCount }}
      orochi.default_shard_count: "{{ .OrochiConfig.DefaultShardCount }}"
{{- end }}
{{- if .OrochiConfig.ChunkInterval }}
      orochi.default_chunk_interval: "{{ .OrochiConfig.ChunkInterval }}"
{{- end }}
{{- if .OrochiConfig.DefaultCompression }}
      orochi.default_compression: "{{ .OrochiConfig.DefaultCompression }}"
{{- end }}
{{- if .OrochiConfig.EnableColumnar }}
      orochi.enable_columnar: "on"
{{- end }}
{{- if .OrochiConfig.CustomParameters }}
{{- range $key, $value := .OrochiConfig.CustomParameters }}
      {{ $key }}: "{{ $value }}"
{{- end }}
{{- end }}
{{- end }}

  storage:
    size: {{ .Storage.Size }}
{{- if .Storage.StorageClass }}
    storageClass: {{ .Storage.StorageClass }}
{{- end }}
    resizeInUseVolumes: {{ boolToString .Storage.ResizeInUseVolumes }}
{{- if .Storage.WALSize }}

  walStorage:
    size: {{ .Storage.WALSize }}
{{- if .Storage.WALStorageClass }}
    storageClass: {{ .Storage.WALStorageClass }}
{{- end }}
{{- end }}

  resources:
    requests:
{{- if .Resources.CPURequest }}
      cpu: "{{ .Resources.CPURequest }}"
{{- end }}
{{- if .Resources.MemoryRequest }}
      memory: "{{ .Resources.MemoryRequest }}"
{{- end }}
    limits:
{{- if .Resources.CPULimit }}
      cpu: "{{ .Resources.CPULimit }}"
{{- end }}
{{- if .Resources.MemoryLimit }}
      memory: "{{ .Resources.MemoryLimit }}"
{{- end }}
{{- if .Resources.HugePages2Mi }}
      hugepages-2Mi: "{{ .Resources.HugePages2Mi }}"
{{- end }}
{{- if .Resources.HugePages1Gi }}
      hugepages-1Gi: "{{ .Resources.HugePages1Gi }}"
{{- end }}

{{- if .Affinity }}
  affinity:
    enablePodAntiAffinity: {{ boolToString .Affinity.EnablePodAntiAffinity }}
{{- if .Affinity.TopologyKey }}
    topologyKey: {{ .Affinity.TopologyKey }}
{{- end }}
{{- if .Affinity.NodeSelector }}
    nodeSelector:
{{ toYaml .Affinity.NodeSelector | indent 6 }}
{{- end }}
{{- if .Affinity.Tolerations }}
    tolerations:
{{- range .Affinity.Tolerations }}
    - key: {{ .Key }}
      operator: {{ .Operator }}
{{- if .Value }}
      value: {{ .Value }}
{{- end }}
{{- if .Effect }}
      effect: {{ .Effect }}
{{- end }}
{{- if .TolerationSeconds }}
      tolerationSeconds: {{ .TolerationSeconds }}
{{- end }}
{{- end }}
{{- end }}
{{- end }}

{{- if .Monitoring }}
{{- if .Monitoring.EnablePodMonitor }}
  monitoring:
    enablePodMonitor: true
{{- end }}
{{- end }}

{{- if .BackupConfig }}
{{- if .BackupConfig.BarmanObjectStore }}
  backup:
    retentionPolicy: "{{ default "30d" .BackupConfig.RetentionPolicy }}"
    barmanObjectStore:
      destinationPath: {{ .BackupConfig.BarmanObjectStore.DestinationPath }}
{{- if .BackupConfig.BarmanObjectStore.Endpoint }}
      endpointURL: {{ .BackupConfig.BarmanObjectStore.Endpoint }}
{{- end }}
{{- if .BackupConfig.BarmanObjectStore.S3Credentials }}
      s3Credentials:
{{- if .BackupConfig.BarmanObjectStore.S3Credentials.InheritFromIAMRole }}
        inheritFromIAMRole: true
{{- else }}
        accessKeyId:
          name: {{ .BackupConfig.BarmanObjectStore.S3Credentials.AccessKeyIDSecret }}
          key: ACCESS_KEY_ID
        secretAccessKey:
          name: {{ .BackupConfig.BarmanObjectStore.S3Credentials.SecretAccessKeySecret }}
          key: SECRET_ACCESS_KEY
{{- end }}
{{- end }}
{{- if .BackupConfig.BarmanObjectStore.WAL }}
      wal:
        compression: {{ .BackupConfig.BarmanObjectStore.WAL.Compression }}
{{- if .BackupConfig.BarmanObjectStore.WAL.MaxParallel }}
        maxParallel: {{ .BackupConfig.BarmanObjectStore.WAL.MaxParallel }}
{{- end }}
{{- end }}
{{- if .BackupConfig.BarmanObjectStore.Data }}
      data:
        compression: {{ .BackupConfig.BarmanObjectStore.Data.Compression }}
{{- if .BackupConfig.BarmanObjectStore.Data.Jobs }}
        jobs: {{ .BackupConfig.BarmanObjectStore.Data.Jobs }}
{{- end }}
        immediateCheckpoint: {{ boolToString .BackupConfig.BarmanObjectStore.Data.ImmediateCheckpoint }}
{{- end }}
{{- end }}
{{- end }}
`

	if _, err := tmpl.New("cluster.yaml.tmpl").Parse(clusterTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse cluster template: %w", err)
	}

	// Backup template
	backupTemplate := `apiVersion: postgresql.cnpg.io/v1
kind: Backup
metadata:
  name: {{ .Name }}
  namespace: {{ .Namespace }}
  labels:
    app.kubernetes.io/name: orochi-db
    app.kubernetes.io/instance: {{ .ClusterName }}
    app.kubernetes.io/component: backup
    app.kubernetes.io/managed-by: orochi-provisioner
    orochi.io/cluster-name: {{ .ClusterName }}
spec:
  cluster:
    name: {{ .ClusterName }}
  method: {{ default "barmanObjectStore" .Method }}
`

	if _, err := tmpl.New("backup.yaml.tmpl").Parse(backupTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse backup template: %w", err)
	}

	// Scheduled backup template
	scheduledBackupTemplate := `apiVersion: postgresql.cnpg.io/v1
kind: ScheduledBackup
metadata:
  name: {{ .Name }}
  namespace: {{ .Namespace }}
  labels:
    app.kubernetes.io/name: orochi-db
    app.kubernetes.io/instance: {{ .ClusterName }}
    app.kubernetes.io/component: scheduled-backup
    app.kubernetes.io/managed-by: orochi-provisioner
    orochi.io/cluster-name: {{ .ClusterName }}
spec:
  schedule: "{{ .Schedule }}"
  immediate: {{ boolToString .Immediate }}
  backupOwnerReference: cluster
  cluster:
    name: {{ .ClusterName }}
`

	if _, err := tmpl.New("scheduled-backup.yaml.tmpl").Parse(scheduledBackupTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse scheduled backup template: %w", err)
	}

	// ServiceMonitor template
	serviceMonitorTemplate := `apiVersion: monitoring.coreos.com/v1
kind: ServiceMonitor
metadata:
  name: {{ .Name }}-monitor
  namespace: {{ .Namespace }}
  labels:
    app.kubernetes.io/name: orochi-db
    app.kubernetes.io/instance: {{ .Name }}
    app.kubernetes.io/component: monitoring
    app.kubernetes.io/managed-by: orochi-provisioner
    orochi.io/cluster-name: {{ .Name }}
spec:
  selector:
    matchLabels:
      cnpg.io/cluster: {{ .Name }}
  endpoints:
  - port: metrics
    interval: {{ default "30s" .ScrapeInterval }}
    path: /metrics
`

	if _, err := tmpl.New("service-monitor.yaml.tmpl").Parse(serviceMonitorTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse service monitor template: %w", err)
	}

	// PgDog deployment template
	pgdogDeploymentTemplate := `apiVersion: apps/v1
kind: Deployment
metadata:
  name: {{ .Name }}-pgdog
  namespace: {{ .Namespace }}
  labels:
    app: pgdog
    app.kubernetes.io/name: pgdog
    app.kubernetes.io/instance: {{ .Name }}
    app.kubernetes.io/component: connection-pooler
    app.kubernetes.io/part-of: orochi-db
    app.kubernetes.io/managed-by: orochi-provisioner
    orochi.io/cluster-name: {{ .Name }}
spec:
  replicas: {{ .Pooler.Replicas }}
  selector:
    matchLabels:
      app: pgdog
      orochi.io/cluster-name: {{ .Name }}
  template:
    metadata:
      labels:
        app: pgdog
        orochi.io/cluster-name: {{ .Name }}
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "9090"
    spec:
      containers:
        - name: pgdog
          image: {{ default "ghcr.io/pgdog/pgdog" .Pooler.Image }}:{{ default "latest" .Pooler.ImageTag }}
          ports:
            - containerPort: 6432
              name: postgres
            - containerPort: 6433
              name: admin
            - containerPort: 9090
              name: metrics
          env:
            - name: PGDOG_CONFIG
              value: "/etc/pgdog/pgdog.toml"
          volumeMounts:
            - name: config
              mountPath: /etc/pgdog
              readOnly: true
          resources:
            requests:
              cpu: "{{ default "250m" .Pooler.Resources.CPURequest }}"
              memory: "{{ default "256Mi" .Pooler.Resources.MemoryRequest }}"
            limits:
              cpu: "{{ default "1000m" .Pooler.Resources.CPULimit }}"
              memory: "{{ default "1Gi" .Pooler.Resources.MemoryLimit }}"
      volumes:
        - name: config
          configMap:
            name: {{ .Name }}-pgdog-config
`

	if _, err := tmpl.New("pgdog-deployment.yaml.tmpl").Parse(pgdogDeploymentTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse pgdog deployment template: %w", err)
	}

	// PgDog configmap template
	pgdogConfigMapTemplate := `apiVersion: v1
kind: ConfigMap
metadata:
  name: {{ .Name }}-pgdog-config
  namespace: {{ .Namespace }}
  labels:
    app: pgdog
    orochi.io/cluster-name: {{ .Name }}
data:
  pgdog.toml: |
    [general]
    host = "0.0.0.0"
    port = 6432
    admin_port = 6433
    connect_timeout = 5000
    query_timeout = 300000
    idle_timeout = {{ default 600 .Pooler.IdleTimeout }}000
    tls = {{ boolToString .Pooler.TLSEnabled }}
    prometheus_enabled = true
    prometheus_port = 9090
    log_level = "info"

    [auth]
    auth_type = "scram-sha-256"

    [pools]
    min_pool_size = {{ default 5 .Pooler.MinPoolSize }}
    max_pool_size = {{ default 100 .Pooler.MaxPoolSize }}
    pool_mode = "{{ default "transaction" .Pooler.Mode }}"
    load_balancing = "least_connections"

    [plugins.query_router]
    enabled = true
    read_write_splitting = {{ boolToString .Pooler.ReadWriteSplit }}

    [[clusters]]
    name = "primary"

      [[clusters.databases]]
      name = "{{ .Name }}"
      host = "{{ .Name }}-rw.{{ .Namespace }}.svc.cluster.local"
      port = 5432
      database = "postgres"
      role = "primary"
`

	if _, err := tmpl.New("pgdog-configmap.yaml.tmpl").Parse(pgdogConfigMapTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse pgdog configmap template: %w", err)
	}

	// PgDog service template
	pgdogServiceTemplate := `apiVersion: v1
kind: Service
metadata:
  name: {{ .Name }}-pgdog
  namespace: {{ .Namespace }}
  labels:
    app: pgdog
    orochi.io/cluster-name: {{ .Name }}
spec:
  type: ClusterIP
  selector:
    app: pgdog
    orochi.io/cluster-name: {{ .Name }}
  ports:
    - name: postgres-direct
      port: 6432
      targetPort: 6432
    - name: admin
      port: 6433
      targetPort: 6433
    - name: metrics
      port: 9090
      targetPort: 9090
---
apiVersion: v1
kind: Service
metadata:
  name: {{ .Name }}-pgdog-headless
  namespace: {{ .Namespace }}
  labels:
    app: pgdog
    orochi.io/cluster-name: {{ .Name }}
spec:
  type: ClusterIP
  clusterIP: None
  selector:
    app: pgdog
    orochi.io/cluster-name: {{ .Name }}
  ports:
    - name: postgres
      port: 6432
      targetPort: 6432
`

	if _, err := tmpl.New("pgdog-service.yaml.tmpl").Parse(pgdogServiceTemplate); err != nil {
		return nil, fmt.Errorf("failed to parse pgdog service template: %w", err)
	}

	return tmpl, nil
}

// RenderCluster renders a cluster template
func (e *TemplateEngine) RenderCluster(spec *types.ClusterSpec) (string, error) {
	return e.render("cluster.yaml.tmpl", spec)
}

// RenderBackup renders a backup template
func (e *TemplateEngine) RenderBackup(data *BackupTemplateData) (string, error) {
	return e.render("backup.yaml.tmpl", data)
}

// RenderScheduledBackup renders a scheduled backup template
func (e *TemplateEngine) RenderScheduledBackup(data *ScheduledBackupTemplateData) (string, error) {
	return e.render("scheduled-backup.yaml.tmpl", data)
}

// RenderServiceMonitor renders a service monitor template
func (e *TemplateEngine) RenderServiceMonitor(data *ServiceMonitorTemplateData) (string, error) {
	return e.render("service-monitor.yaml.tmpl", data)
}

// render renders a template with the given data
func (e *TemplateEngine) render(templateName string, data interface{}) (string, error) {
	var buf bytes.Buffer

	tmpl := e.templates.Lookup(templateName)
	if tmpl == nil {
		return "", fmt.Errorf("template %s not found", templateName)
	}

	if err := tmpl.Execute(&buf, data); err != nil {
		return "", fmt.Errorf("failed to execute template %s: %w", templateName, err)
	}

	return buf.String(), nil
}

// BackupTemplateData holds data for backup templates
type BackupTemplateData struct {
	Name        string
	Namespace   string
	ClusterName string
	Method      string
}

// ScheduledBackupTemplateData holds data for scheduled backup templates
type ScheduledBackupTemplateData struct {
	Name        string
	Namespace   string
	ClusterName string
	Schedule    string
	Immediate   bool
}

// ServiceMonitorTemplateData holds data for service monitor templates
type ServiceMonitorTemplateData struct {
	Name           string
	Namespace      string
	ScrapeInterval string
}

// PgDogTemplateData holds data for PgDog templates
type PgDogTemplateData struct {
	Name        string
	Namespace   string
	Pooler      *types.ConnectionPoolerSpec
	Labels      map[string]string
	Annotations map[string]string
}

// RenderPgDogDeployment renders the PgDog deployment template
func (e *TemplateEngine) RenderPgDogDeployment(data *PgDogTemplateData) (string, error) {
	return e.render("pgdog-deployment.yaml.tmpl", data)
}

// RenderPgDogConfigMap renders the PgDog configmap template
func (e *TemplateEngine) RenderPgDogConfigMap(data *PgDogTemplateData) (string, error) {
	return e.render("pgdog-configmap.yaml.tmpl", data)
}

// RenderPgDogService renders the PgDog service template
func (e *TemplateEngine) RenderPgDogService(data *PgDogTemplateData) (string, error) {
	return e.render("pgdog-service.yaml.tmpl", data)
}
