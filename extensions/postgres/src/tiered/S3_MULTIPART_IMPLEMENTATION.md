# S3 Multipart Upload Implementation

This document describes the S3 multipart upload implementation for Orochi DB's tiered storage system.

## Files

- `src/tiered/s3_multipart.c` - Implementation of S3 multipart upload functions
- `src/tiered/tiered_storage.h` - Function declarations (already present)
- `src/tiered/tiered_storage.c` - Main tiered storage implementation

## Implemented Functions

### 1. `s3_multipart_upload_start()`

**Purpose**: Initiate a multipart upload session

**API**: `POST /{bucket}/{key}?uploads`

**Returns**: Upload ID string to be used for subsequent part uploads

**Implementation Details**:
- Creates AWS Signature V4 authentication headers
- Sends POST request with empty body
- Parses UploadId from XML response
- Example response:
  ```xml
  <InitiateMultipartUploadResult>
    <UploadId>upload-id-string</UploadId>
  </InitiateMultipartUploadResult>
  ```

### 2. `s3_multipart_upload_part()`

**Purpose**: Upload a single part of the multipart upload

**API**: `PUT /{bucket}/{key}?partNumber={n}&uploadId={id}`

**Parameters**:
- `part_number`: 1-10000 (part number)
- `data`: Binary data for this part
- `size`: Size in bytes (minimum 5MB except for last part)

**Returns**: `true` on success, `false` on failure

**Implementation Details**:
- Validates part number (1-10000)
- Checks minimum size requirement (5MB except last part)
- Creates AWS Signature V4 for PUT request with payload
- Uploads part data
- S3 returns ETag in response headers (needed for completion)

### 3. `s3_multipart_upload_complete()`

**Purpose**: Finalize the multipart upload

**API**: `POST /{bucket}/{key}?uploadId={id}`

**Payload**: XML document listing all parts with their ETags

**Returns**: `true` on success, `false` on failure

**Implementation Details**:
- Builds CompleteMultipartUpload XML payload:
  ```xml
  <CompleteMultipartUpload>
    <Part>
      <PartNumber>1</PartNumber>
      <ETag>"etag-value"</ETag>
    </Part>
    <!-- More parts... -->
  </CompleteMultipartUpload>
  ```
- **NOTE**: Current implementation sends empty XML as a placeholder
- **TODO**: In production, must track ETags from part uploads and include them

### 4. `s3_multipart_upload_abort()`

**Purpose**: Cancel multipart upload and delete all uploaded parts

**API**: `DELETE /{bucket}/{key}?uploadId={id}`

**Implementation Details**:
- Sends DELETE request to clean up partial upload
- Called when upload fails or is explicitly cancelled
- Frees S3 storage used by uploaded parts

### 5. `orochi_upload_chunk_to_s3()`

**Purpose**: High-level helper that automatically chooses upload method

**Logic**:
- Files < 100MB: Use regular `s3_upload()` (single PUT request)
- Files >= 100MB: Use multipart upload with 10MB parts

**Benefits**:
- Transparent to caller
- Optimizes for file size
- Handles large files efficiently
- Automatic error handling and abort on failure

## Upload Flow

### Small File (< 100MB)
```
orochi_upload_chunk_to_s3()
  └─> s3_upload()  [Single PUT request]
```

### Large File (>= 100MB)
```
orochi_upload_chunk_to_s3()
  ├─> s3_multipart_upload_start()     [Initiate: POST ?uploads]
  ├─> s3_multipart_upload_part() x N  [Upload parts: PUT ?partNumber=N&uploadId=...]
  └─> s3_multipart_upload_complete()  [Finalize: POST ?uploadId=...]
       OR
      s3_multipart_upload_abort()      [On failure: DELETE ?uploadId=...]
```

## AWS Signature V4 Authentication

All requests use the existing `generate_aws_signature_v4()` function from `tiered_storage.c`:

```c
char *generate_aws_signature_v4(S3Client *client, const char *method,
                               const char *uri, const char *query_string,
                               const char *payload, size_t payload_len,
                               const char *date_stamp, const char *amz_date,
                               char **out_authorization);
```

This generates:
- Canonical request
- String to sign
- Signing key (HMAC-SHA256 chain)
- Authorization header

## Configuration

Uses existing S3 configuration from GUC variables:
- `orochi_s3_bucket` - S3 bucket name
- `orochi_s3_access_key` - AWS access key ID
- `orochi_s3_secret_key` - AWS secret access key
- `orochi_s3_region` - AWS region (e.g., "us-east-1")
- `orochi_s3_endpoint` - S3 endpoint URL

## Example Usage

```c
/* Create S3 client */
OrochiS3Config config = {
    .endpoint = "s3.amazonaws.com",
    .bucket = "my-bucket",
    .access_key = "AKIAIOSFODNN7EXAMPLE",
    .secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
    .region = "us-east-1",
    .use_ssl = true,
    .connection_timeout = 30000
};
S3Client *client = s3_client_create(&config);

/* Upload chunk (automatically uses multipart if >= 100MB) */
S3UploadResult *result = orochi_upload_chunk_to_s3(
    client,
    "orochi/chunks/12345.columnar",
    chunk_data,
    chunk_size
);

if (result->success) {
    elog(LOG, "Upload successful: %s", result->etag);
} else {
    elog(ERROR, "Upload failed: %s", result->error_message);
}

s3_client_destroy(client);
```

## Testing

To test the multipart upload implementation:

```sql
-- Configure S3
SET orochi.s3_bucket = 'my-test-bucket';
SET orochi.s3_access_key = 'AKIAIOSFODNN7EXAMPLE';
SET orochi.s3_secret_key = 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY';
SET orochi.s3_region = 'us-east-1';

-- Create hypertable
CREATE TABLE metrics (
    time TIMESTAMPTZ NOT NULL,
    device_id INT,
    value DOUBLE PRECISION
);
SELECT orochi.create_hypertable('metrics', 'time');

-- Insert large amount of data to create big chunks
INSERT INTO metrics
SELECT time, device_id, random()
FROM generate_series('2024-01-01'::timestamptz, '2024-12-31'::timestamptz, '1 second') AS time,
     generate_series(1, 1000) AS device_id;

-- Move chunk to cold storage (will use multipart upload if > 100MB)
SELECT orochi.move_chunk_to_tier(chunk_id, 'cold')
FROM orochi.orochi_chunks
WHERE hypertable_oid = 'metrics'::regclass
LIMIT 1;
```

## Performance Characteristics

### Regular Upload (< 100MB)
- Single HTTP request
- Fast for small files
- Limited by single connection bandwidth

### Multipart Upload (>= 100MB)
- Multiple parallel part uploads possible (not yet implemented)
- Better for large files
- Resumable (can retry individual parts)
- Overhead: 3 requests minimum (initiate + 1 part + complete)

### Current Configuration
- Threshold: 100MB
- Part size: 10MB
- Sequential part uploads

### Future Optimizations
- Parallel part uploads using multiple connections
- Dynamic part size based on total file size
- Progress tracking and reporting
- ETag storage for proper completion

## Limitations and Future Work

### Current Limitations
1. **No ETag tracking**: `s3_multipart_upload_complete()` sends empty XML
   - Must implement ETag storage and include in completion request
   - Suggestion: Add List structure to track `{part_number, etag}` pairs

2. **Sequential uploads**: Parts uploaded one at a time
   - Could parallelize with multiple CURL handles
   - Would significantly improve throughput for large files

3. **No resume support**: Failed uploads must restart from beginning
   - S3 supports resuming with existing UploadId
   - Need to persist UploadId and completed parts

4. **Fixed part size**: Always uses 10MB parts
   - Could optimize based on total size (e.g., 100MB parts for TB files)

### Suggested Improvements

#### 1. ETag Tracking
```c
typedef struct S3UploadContext {
    char *upload_id;
    List *parts;  /* List of S3UploadPart */
    int next_part_number;
} S3UploadContext;

typedef struct S3UploadPart {
    int part_number;
    char *etag;
    int64 size;
} S3UploadPart;
```

#### 2. Parallel Uploads
```c
/* Use CURL multi interface for parallel uploads */
CURLM *multi_handle = curl_multi_init();
/* Add multiple part uploads to multi handle */
/* Process them in parallel */
```

#### 3. Progress Callback
```c
typedef void (*S3UploadProgressCallback)(
    int64 bytes_uploaded,
    int64 total_bytes,
    void *user_data
);

S3UploadResult *orochi_upload_chunk_to_s3_with_progress(
    S3Client *client,
    const char *key,
    const char *data,
    int64 size,
    S3UploadProgressCallback callback,
    void *callback_data
);
```

## Memory Management

All functions use PostgreSQL memory contexts:
- `palloc()` / `pfree()` for memory allocation
- Memory context cleanup on error via `ereport(ERROR, ...)`
- CURL buffers use `repalloc()` for dynamic growth

## Error Handling

All functions check:
- NULL parameters
- CURL initialization failures
- HTTP status codes (200-299 = success)
- Response parsing errors
- Memory allocation failures

Errors are logged using PostgreSQL's `elog()`:
- `LOG`: Successful operations
- `WARNING`: Failed operations with details
- `DEBUG1`: Verbose diagnostic information

## References

- [AWS S3 Multipart Upload API](https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html)
- [AWS Signature V4 Signing Process](https://docs.aws.amazon.com/general/latest/gr/signature-version-4.html)
- [libcurl Documentation](https://curl.se/libcurl/c/)
- [PostgreSQL Extension Development](https://www.postgresql.org/docs/current/extend.html)

## Build Integration

The implementation is integrated into the build system:

**Makefile changes**:
```makefile
OBJS = \
    ...
    src/tiered/tiered_storage.o \
    src/tiered/s3_multipart.o \  # Added
    ...
```

**Header declarations**: Already present in `src/tiered/tiered_storage.h`

**Linker flags**: Uses existing `-lcurl -lssl -lcrypto`
