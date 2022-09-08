FROM alpine:3.16.2

WORKDIR /OHIE

RUN apt add --no-cache libssl-dev
RUN apt add --no-cache libboost-all-dev

COPY ./ ./

CMD make

ENTRYPOINT [ "./Node"]
