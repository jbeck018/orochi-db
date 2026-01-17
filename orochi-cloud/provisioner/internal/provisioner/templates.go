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

	"github.com/orochi-db/orochi-cloud/provisioner/pkg/types"
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
