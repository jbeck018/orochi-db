package output

import (
	"encoding/json"
	"fmt"
	"os"
)

// JSON outputs data as formatted JSON.
func JSON(data interface{}) error {
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(data)
}

// JSONCompact outputs data as compact JSON.
func JSONCompact(data interface{}) error {
	return json.NewEncoder(os.Stdout).Encode(data)
}

// JSONString returns data as a JSON string.
func JSONString(data interface{}) (string, error) {
	bytes, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return "", err
	}
	return string(bytes), nil
}

// PrintJSON prints any data as JSON.
func PrintJSON(data interface{}) {
	if err := JSON(data); err != nil {
		Error("Failed to format JSON: %v", err)
	}
}

// FormatOutput outputs data in the specified format.
func FormatOutput(format string, data interface{}, tableFunc func()) {
	switch format {
	case "json":
		PrintJSON(data)
	case "table", "":
		tableFunc()
	default:
		Error("Unknown output format: %s", format)
		fmt.Println("Supported formats: table, json")
	}
}

// ResultWrapper wraps a result with a status for JSON output.
type ResultWrapper struct {
	Status string      `json:"status"`
	Data   interface{} `json:"data,omitempty"`
	Error  string      `json:"error,omitempty"`
}

// PrintResult prints a result with status.
func PrintResult(format string, success bool, data interface{}, errMsg string) {
	if format == "json" {
		result := ResultWrapper{
			Data: data,
		}
		if success {
			result.Status = "success"
		} else {
			result.Status = "error"
			result.Error = errMsg
		}
		PrintJSON(result)
		return
	}

	// Table format
	if success {
		if data != nil {
			fmt.Printf("%v\n", data)
		}
	} else {
		Error(errMsg)
	}
}

// MessageResponse represents a simple message response.
type MessageResponse struct {
	Message string `json:"message"`
}

// PrintMessage prints a message in the appropriate format.
func PrintMessage(format string, message string) {
	if format == "json" {
		PrintJSON(MessageResponse{Message: message})
	} else {
		Print(message)
	}
}

// PrintSuccess prints a success message in the appropriate format.
func PrintSuccess(format string, message string) {
	if format == "json" {
		PrintJSON(ResultWrapper{Status: "success", Data: MessageResponse{Message: message}})
	} else {
		Success(message)
	}
}

// PrintError prints an error message in the appropriate format.
func PrintError(format string, message string) {
	if format == "json" {
		PrintJSON(ResultWrapper{Status: "error", Error: message})
	} else {
		Error(message)
	}
}
