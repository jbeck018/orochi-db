# S3 Functions Implementation

## Overview

This document describes the implementation of two missing S3 client functions in the Orochi DB tiered storage system:

1. **`s3_list_objects()`** - List objects in S3 with a given prefix
2. **`s3_head_object()`** - Get metadata for a specific S3 object

## Files Modified

### `/extensions/postgres/src/tiered/tiered_storage.h`

Updated the `S3Object` struct to include additional metadata fields:

```c
typedef struct S3Object
{
    char               *key;
    char               *etag;
    int64               size;
    TimestampTz         last_modified;
    char               *content_type;      // NEW
    char               *storage_class;     // NEW
} S3Object;
```

### `/extensions/postgres/src/tiered/tiered_storage.c`

Added the following components:

#### 1. Constants and Helper Structures

```c
#define POSTGRES_EPOCH_JDATE    2451545     /* Julian date for timestamp conversion */
#define EMPTY_SHA256_HASH       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

typedef struct HeaderCallbackData
{
    char               *etag;
    int64               size;
    TimestampTz         last_modified;
    char               *content_type;
    char               *storage_class;
} HeaderCallbackData;
```

#### 2. Header Callback Function

```c
static size_t orochi_curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
```

Extracts HTTP response headers including:
- ETag (with quote removal)
- Content-Length
- Last-Modified (parsed from HTTP date format)
- Content-Type
- x-amz-storage-class

#### 3. s3_head_object() Implementation

```c
S3Object *s3_head_object(S3Client *client, const char *key)
```

**Purpose**: Get metadata for a specific S3 object without downloading content.

**Method**: HTTP HEAD request

**Authentication**: AWS Signature V4

**Return Values**:
- Non-NULL `S3Object*` with metadata if object exists (HTTP 200)
- NULL if object doesn't exist (HTTP 404)
- NULL on error (logs warning)

**Extracted Metadata**:
- Object key
- ETag (MD5 hash or multipart upload ID)
- Size in bytes
- Last modified timestamp
- Content type (defaults to "application/octet-stream")
- Storage class (defaults to "STANDARD")

**Error Handling**:
- Returns NULL for invalid parameters
- Returns NULL if CURL initialization fails
- Returns NULL on network errors
- Logs DEBUG1 on success, WARNING on error

#### 4. XML Parser Helper

```c
static char *extract_xml_text(const char *tag, const char **next_pos)
```

Simple XML tag extractor for parsing S3 ListObjectsV2 responses.

**Features**:
- Searches for `<tag>...</tag>` pattern
- Updates `next_pos` to continue parsing
- Returns NULL if tag not found
- Memory allocated with `palloc()`

#### 5. s3_list_objects() Implementation

```c
List *s3_list_objects(S3Client *client, const char *prefix)
```

**Purpose**: List all objects in S3 bucket with optional prefix filter.

**Method**: HTTP GET with ListObjectsV2 API

**Query Parameters**:
- `list-type=2` (use ListObjectsV2)
- `prefix=<prefix>` (optional filter)
- `continuation-token=<token>` (for pagination)

**Pagination Support**:
- Automatically handles pagination for >1000 objects
- Extracts `NextContinuationToken` from XML response
- Loops until `IsTruncated` is false
- No limit on total objects returned

**XML Parsing**:
- Parses `<IsTruncated>` to check for more results
- Extracts `<NextContinuationToken>` for next page
- Iterates through `<Contents>` elements
- Extracts Key, Size, ETag, LastModified, StorageClass

**ETag Processing**:
- Removes surrounding quotes from ETag values
- S3 returns ETags as `"abc123"`, stored as `abc123`

**Timestamp Conversion**:
- Parses ISO 8601 format: `2025-01-17T12:34:56.000Z`
- Converts to PostgreSQL `TimestampTz` (microseconds since 2000-01-01)
- Uses `strptime()` and `timegm()` for conversion

**Return Value**:
- PostgreSQL `List*` of `S3Object*` pointers
- Empty list (`NIL`) on error or no objects
- Logs total object count at DEBUG1 level

**Memory Management**:
- All strings allocated with `palloc()`
- Objects added to list with `lappend()`
- Response buffers freed after each page
- Continuation token freed when done

## AWS Signature V4 Integration

Both functions use the existing `generate_aws_signature_v4()` infrastructure:

1. Generate current timestamp in AWS format (`YYYYMMDDTHHMMSSZ`)
2. Build canonical request with method, URI, query string, headers
3. Calculate signing key using secret key, date, region, service
4. Sign the request and build Authorization header
5. Include required headers:
   - `Host`
   - `x-amz-date`
   - `x-amz-content-sha256`
   - `Authorization`

## Usage Examples

### List Objects with Prefix

```c
S3Client *client = s3_client_create(config);

// List all chunks
List *objects = s3_list_objects(client, "orochi/chunks/");

ListCell *lc;
foreach(lc, objects)
{
    S3Object *obj = (S3Object *)lfirst(lc);
    elog(LOG, "Found: %s (size=%ld, storage=%s)",
         obj->key, obj->size, obj->storage_class);
}

s3_client_destroy(client);
```

### Get Object Metadata

```c
S3Client *client = s3_client_create(config);

S3Object *obj = s3_head_object(client, "orochi/chunks/12345.columnar");

if (obj != NULL)
{
    elog(LOG, "Object exists: size=%ld, etag=%s, modified=%ld",
         obj->size, obj->etag, obj->last_modified);

    // Use the metadata...

    // Free when done
    pfree(obj->key);
    pfree(obj->etag);
    pfree(obj->content_type);
    pfree(obj->storage_class);
    pfree(obj);
}
else
{
    elog(WARNING, "Object not found");
}

s3_client_destroy(client);
```

### Check if Object Exists

```c
// Using head_object (more metadata)
S3Object *obj = s3_head_object(client, key);
bool exists = (obj != NULL);
if (obj) {
    // Free metadata
    pfree(obj->key);
    pfree(obj->etag);
    pfree(obj->content_type);
    pfree(obj->storage_class);
    pfree(obj);
}

// Or use existing s3_object_exists() for simple boolean check
bool exists = s3_object_exists(client, key);
```

## Testing

A unit test suite was created at `/extensions/postgres/test/unit/test_s3_functions.c`:

```bash
cd extensions/postgres/test/unit
gcc -std=c11 -Wall -Wextra -o test_s3_functions test_s3_functions.c
./test_s3_functions
```

**Tests Included**:
- XML tag parsing
- Truncation flag parsing
- Continuation token extraction
- Multiple `<Contents>` elements
- ETag quote removal

**Test Results**: All tests passed ✓

## Performance Considerations

### s3_list_objects()

- **Network Overhead**: Makes multiple HTTP requests for large buckets (>1000 objects)
- **Memory Usage**: Holds all objects in memory; consider pagination for huge buckets
- **Response Size**: Initial buffer is 64KB, grows as needed
- **Optimization**: Use specific prefixes to limit result set

### s3_head_object()

- **Minimal Network**: HEAD request only transfers headers, not content
- **Fast**: Typical response time <100ms for nearby S3 regions
- **Efficient**: No data transfer, only metadata
- **Use Case**: Prefer over GET when only metadata needed

## Error Handling

Both functions handle errors gracefully:

1. **Invalid Parameters**: Return NULL/NIL
2. **CURL Errors**: Log warning, return NULL/NIL
3. **HTTP Errors**: Log warning with status code
4. **404 Not Found**: Return NULL (not an error for head_object)
5. **Network Timeout**: Respects `client->timeout_ms`

## Security

- Uses AWS Signature V4 for authentication
- Supports SSL/TLS via `client->use_ssl`
- Never logs sensitive credentials
- Credentials stored securely in S3Client struct

## Future Enhancements

Potential improvements:

1. **Parallel Pagination**: Fetch multiple pages concurrently
2. **Result Streaming**: Callback-based iteration for huge lists
3. **Metadata Caching**: Cache HEAD responses to reduce API calls
4. **Batch HEAD**: Use S3 Batch API for multiple objects
5. **Conditional Requests**: Support If-Modified-Since, If-None-Match
6. **Custom Metadata**: Extract x-amz-meta-* headers

## Compatibility

- **PostgreSQL**: 16, 17, 18
- **S3 API**: Compatible with Amazon S3 and S3-compatible services (MinIO, Ceph, etc.)
- **CURL**: Requires libcurl with SSL support
- **OpenSSL**: Required for HMAC-SHA256 signing

## References

- [AWS S3 ListObjectsV2 API](https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjectsV2.html)
- [AWS S3 HeadObject API](https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadObject.html)
- [AWS Signature Version 4](https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html)
- [PostgreSQL Memory Contexts](https://www.postgresql.org/docs/current/xfunc-c.html#XFUNC-C-MEMORY)
