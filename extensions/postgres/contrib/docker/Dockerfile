ARG PG_MAJOR=18
FROM postgres:${PG_MAJOR}

# Install build dependencies
# Supports PostgreSQL 16, 17, and 18
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-${PG_MAJOR} \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    librdkafka-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Copy extension source
COPY . /orochi
WORKDIR /orochi

# Build and install extension
RUN make clean && make && make install

# Enable extension in PostgreSQL
RUN echo "shared_preload_libraries = 'orochi'" >> /usr/share/postgresql/postgresql.conf.sample

# Create initialization script
COPY contrib/docker/init.sql /docker-entrypoint-initdb.d/

EXPOSE 5432
