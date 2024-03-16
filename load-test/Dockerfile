FROM php:8.3-alpine

RUN apk add --no-cache \
	$PHPIZE_DEPS \
    && apk add --no-cache postgresql-dev \
	&& docker-php-ext-install pdo pdo_pgsql \
	&& pecl install swoole

RUN docker-php-ext-enable swoole

COPY . /app
WORKDIR /app

CMD ["php", "./index.php"]