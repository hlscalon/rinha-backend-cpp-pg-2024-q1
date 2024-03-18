# primeiro builda o projeto
FROM alpine:3 AS build

RUN apk update && \
    apk add --no-cache \
    build-base \
	linux-headers \
    # cmake \
    postgresql-dev \
    libpq-dev \
    python3 \
    git

WORKDIR /usr/app

COPY src/* ./src/
COPY src/libs/* ./src/libs/

# instala o libpqxx
RUN cd src/libs && \
    git clone https://github.com/jtv/libpqxx && \
    cd libpqxx && \
    ./configure --disable-shared && \
    make && \
    make install

COPY Makefile ./

RUN make prod

# depois roda
FROM alpine:3

RUN apk update && \
    apk add --no-cache \
	libstdc++ \
    libpq-dev

WORKDIR /usr/app

COPY --from=build /usr/app .

CMD ["/usr/app/server"]
