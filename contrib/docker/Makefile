# Targets that end in -build create containers that will build libral automatically when they are run. They have the necessary tooling to do that in them, but libral sources will be downloaded every time

f25-build:
	docker build -f Dockerfile.f25-build -t libral/f25-build .

f25:
	docker build -f Dockerfile.f25 -t libral/f25 .

el6-build-static:
	docker build -f Dockerfile.el6-build-static -t libral/el6-build-static .

el6-build-static-go: el6-build-static
	docker build -f Dockerfile.el6-build-static-go -t libral/el6-build-static-go .

clean:
	@C="$$(docker ps -a -q -f status=exited)"; \
	[ -n "$$C" ] && docker rm $$C || : ; \
	I="$$(docker images -f "dangling=true" -q)"; \
	[ -n "$$I" ] && docker rmi $$I || :

.PHONY: clean
