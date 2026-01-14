FROM postgres:16

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-16 \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev \
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
