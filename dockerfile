FROM ubuntu:latest

RUN apt-get update && apt-get install -y g++

WORKDIR /app

COPY . /app

RUN g++ -o coordinator coordinator.cpp -pthread

ENTRYPOINT ["./coordinator"]
