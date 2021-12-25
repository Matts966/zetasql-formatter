update: osx linux push
	@echo "all artifacts are updated"
run: build
	docker run -it --rm -v `pwd`:/home:Z matts966/zetasql-formatter:latest
build:
	DOCKER_BUILDKIT=1 docker build -t matts966/zetasql-formatter:latest -f ./docker/Dockerfile .
build-formatter: build
	mv ./zetasql-kotlin/build/*_jar.jar ~/.Trash/
	docker run -it --rm -v `pwd`:/work/zetasql/ \
		-v /var/run/docker.sock:/var/run/docker.sock \
		bazel
push: build
	docker push matts966/zetasql-formatter:latest
osx:
	CC=g++ bazelisk build //zetasql/tools/zetasql-formatter:format
	sudo cp ./bazel-bin/zetasql/tools/zetasql-formatter/format ./zetasql-formatter
	sudo cp ./zetasql-formatter /usr/local/bin
linux: build
	./docker/linux-copy-bin.sh
test:
		CC=g++ bazelisk test --test_output=errors //zetasql/tools/...
.PHONY: run build build-formatter osx push test
