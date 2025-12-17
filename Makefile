IMAGE_NAME := openslide-base-builder
WORKDIR := $(shell pwd)

.PHONY: build run all

all: run

build:
	docker build -t $(IMAGE_NAME) .

run: build
	docker run --rm \
		-v "$(WORKDIR)":/openslide \
		-w /openslide \
		-e USER_ID=$(shell id -u) \
		-e USER_GROUP=$(shell id -g) \
		$(IMAGE_NAME)
	chown -R $(shell id -u):$(shell id -g) .