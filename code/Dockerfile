FROM alpine:3.16.2

WORKDIR /OHIE

RUN apk add --no-cache openssl-dev boost boost-dev build-base

COPY ./ ./

RUN make

ENTRYPOINT [ "./Node"]
