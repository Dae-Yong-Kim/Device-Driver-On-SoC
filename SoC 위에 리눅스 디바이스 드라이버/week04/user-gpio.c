#include <gpiod.h> // libgpiod 헤더 파일을 include 함
#include <stdio.h>
#include <unistd.h>

int main() {
    // /dev에 생긴 GPIO chip의 디바이스 노드 파일 경로로 열기
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    struct gpiod_line *line[4];
    int i;

    for (i = 0; i < sizeof(line) / sizeof(*line); i++) {
         // GPIO 핀 0~3을 얻어 오기
         line[i] = gpiod_chip_get_line(chip, i);
    }
    // GPIO 핀 0 ~ 2는 출력으로 설정하고 초기값은 1로 설정
    gpiod_line_request_output(line[0], NULL, 1);
    gpiod_line_request_output(line[1], NULL, 1);
    gpiod_line_request_output(line[2], NULL, 1);
    // GPIO 핀 3은 입력으로 설정
    gpiod_line_request_input(line[3], NULL);
    // GPIO 핀 3의 값을 읽어오기
    printf("The current key status - %d\n", gpiod_line_get_value(line[3]));

    sleep(5);

    // GPIO 핀 0~2의 값을 모두 0으로 설정
    gpiod_line_set_value(line[0], 0);
    gpiod_line_set_value(line[1], 0);
    gpiod_line_set_value(line[2], 0);
    // GPIO 핀 3의 값을 읽어오기
    printf("The current key status - %d\n", gpiod_line_get_value(line[3]));

    return 0;
}

