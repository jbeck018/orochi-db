// Package session handles PostgreSQL session variable injection.
package session

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"strings"
	"time"

	"github.com/orochi-db/pgdog-jwt-gateway/internal/auth"
)

// Injector handles injection of session variables into PostgreSQL connections.
type Injector struct {
	// Timeout for session variable injection operations.
	Timeout time.Duration
}

// NewInjector creates a new session variable injector.
func NewInjector(timeout time.Duration) *Injector {
	return &Injector{
		Timeout: timeout,
	}
}

// InjectClaims injects the JWT claims as PostgreSQL session variables.
// This should be called after successful authentication and connection establishment.
func (i *Injector) InjectClaims(conn net.Conn, claims *auth.OrochiClaims) error {
	if claims == nil {
		return nil
	}

	vars := claims.GetSessionVariables()
	if len(vars) == 0 {
		return nil
	}

	// Build SET commands for all session variables
	for name, value := range vars {
		if err := i.setSessionVariable(conn, name, value); err != nil {
			return fmt.Errorf("failed to set %s: %w", name, err)
		}
	}

	return nil
}

// setSessionVariable sends a SET command and waits for confirmation.
func (i *Injector) setSessionVariable(conn net.Conn, name, value string) error {
	// Escape the value to prevent SQL injection
	escapedValue := escapeString(value)

	// Build the query: SET name = 'value'
	query := fmt.Sprintf("SET %s = '%s'", name, escapedValue)

	// Send as a simple query message
	if err := i.sendQuery(conn, query); err != nil {
		return err
	}

	// Read and validate response
	return i.readQueryResponse(conn)
}

// sendQuery sends a PostgreSQL simple query message.
func (i *Injector) sendQuery(conn net.Conn, query string) error {
	if i.Timeout > 0 {
		conn.SetWriteDeadline(time.Now().Add(i.Timeout))
		defer conn.SetWriteDeadline(time.Time{})
	}

	// Query message format:
	// - 1 byte: 'Q' message type
	// - 4 bytes: message length (including self, not including type)
	// - N bytes: query string (null-terminated)

	queryBytes := []byte(query)
	msgLen := 4 + len(queryBytes) + 1 // length field + query + null terminator

	msg := make([]byte, 1+msgLen)
	msg[0] = 'Q'
	binary.BigEndian.PutUint32(msg[1:5], uint32(msgLen))
	copy(msg[5:], queryBytes)
	msg[len(msg)-1] = 0 // null terminator

	_, err := conn.Write(msg)
	return err
}

// readQueryResponse reads the response to a simple query.
func (i *Injector) readQueryResponse(conn net.Conn) error {
	if i.Timeout > 0 {
		conn.SetReadDeadline(time.Now().Add(i.Timeout))
		defer conn.SetReadDeadline(time.Time{})
	}

	// Read messages until we get ReadyForQuery ('Z')
	for {
		// Read message type
		msgType := make([]byte, 1)
		if _, err := io.ReadFull(conn, msgType); err != nil {
			return fmt.Errorf("failed to read message type: %w", err)
		}

		// Read message length
		lenBuf := make([]byte, 4)
		if _, err := io.ReadFull(conn, lenBuf); err != nil {
			return fmt.Errorf("failed to read message length: %w", err)
		}
		msgLen := int(binary.BigEndian.Uint32(lenBuf)) - 4

		// Read message body
		if msgLen > 0 {
			body := make([]byte, msgLen)
			if _, err := io.ReadFull(conn, body); err != nil {
				return fmt.Errorf("failed to read message body: %w", err)
			}

			// Check for error response
			if msgType[0] == 'E' {
				return parseErrorResponse(body)
			}
		}

		// ReadyForQuery means we're done
		if msgType[0] == 'Z' {
			return nil
		}

		// CommandComplete, RowDescription, DataRow, etc. - continue reading
	}
}

// parseErrorResponse extracts error message from PostgreSQL error response.
func parseErrorResponse(body []byte) error {
	// Error response format: series of field type + null-terminated string
	var message string
	var detail string
	var code string

	i := 0
	for i < len(body) {
		if body[i] == 0 {
			break
		}

		fieldType := body[i]
		i++

		// Find null terminator
		end := i
		for end < len(body) && body[end] != 0 {
			end++
		}
		fieldValue := string(body[i:end])
		i = end + 1

		switch fieldType {
		case 'M': // Message
			message = fieldValue
		case 'D': // Detail
			detail = fieldValue
		case 'C': // Code
			code = fieldValue
		}
	}

	if detail != "" {
		return fmt.Errorf("PostgreSQL error %s: %s (%s)", code, message, detail)
	}
	return fmt.Errorf("PostgreSQL error %s: %s", code, message)
}

// escapeString escapes a string for safe use in PostgreSQL.
func escapeString(s string) string {
	// Replace single quotes with doubled single quotes
	return strings.ReplaceAll(s, "'", "''")
}

// BuildSetCommands creates SQL SET commands for the given claims.
// This can be used to batch all SET commands into a single query.
func BuildSetCommands(claims *auth.OrochiClaims) string {
	if claims == nil {
		return ""
	}

	vars := claims.GetSessionVariables()
	if len(vars) == 0 {
		return ""
	}

	var commands []string
	for name, value := range vars {
		commands = append(commands, fmt.Sprintf("SET %s = '%s'", name, escapeString(value)))
	}

	return strings.Join(commands, "; ")
}
