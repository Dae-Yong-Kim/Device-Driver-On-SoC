#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "sysemu/runstate.h"
#include "qapi/visitor.h"
#include "chardev/char-fe.h"

#define TYPE_GPIO_COMENTO "leds-and-button"
OBJECT_DECLARE_SIMPLE_TYPE(GPIO_COMENTO_State, GPIO_COMENTO)

struct GPIO_COMENTO_State {
    SysBusDevice parent_obj;
    int64_t key_state;
    qemu_irq key_pin;
};

// GPIO chip에서 보낸 신호가 level로 들어옴 (High – 1, Low – 0)
static void gpio_comento_handler(void *opaque, int n, int level)
{
    // 0 : 검정, 1 : 빨강, 2 : 초록, 3 : 노랑, 4: 파랑
    if (++ n == 3) {
        n = 4;
    }
    if (level) {        
        // 쉘에서 색상을 출력하기 위한 방법 이용
        // \033[0;3<색상> : 해당 색상으로 설정, \033[0m : 원복
        printf("\033[0;3%dmLED ON\033[0m\n", n);
    } else {
        printf("\033[1;3%dmLED OFF\033[0m\n", n);
    }
}

// QMP로 key 속성을 얻어오려고 할 때 문자열로 반환
static char* gpio_comento_get_key(Object *obj, Error **errp)
{
    GPIO_COMENTO_State *s = GPIO_COMENTO(obj);
    return s->key_state? g_strdup("True") : g_strdup("False");
}
// QMP로 key 속성을 문자열로 넣었을 때 GPIO 핀 설정
static void gpio_comento_set_key(Object *obj, const char *value,
                                 Error **errp)
{
    GPIO_COMENTO_State *s = GPIO_COMENTO(obj);
    if (g_strcmp0(value, "true") && g_strcmp0(value, "True")) {
        s->key_state = 0;
         qemu_irq_lower(s->key_pin); // GPIO 핀을 High로 설정
    } else {
        s->key_state = 1;
         qemu_irq_raise(s->key_pin); // GPIO 핀을 Low로 설정
    }
}
static void gpio_comento_init(Object *obj)
{
    GPIO_COMENTO_State *s = GPIO_COMENTO(obj);
    DeviceState *dev = DEVICE(obj);
    // 0번 부터 3개의 핀(0 ~ 2)은 GPIO 출력 핀 (하드웨어 입장에서는 입력)
    qdev_init_gpio_in(dev, gpio_comento_handler, 3);
    // 3 부터 1개의 핀(3)은 GPIO 입력 핀 (하드웨어 입장에서는 출력)
    qdev_init_gpio_out(dev, &s->key_pin, 1);
    s->key_state = 0;
    object_property_add_str(obj, "key",
                        gpio_comento_get_key, gpio_comento_set_key);
}
static const TypeInfo gpio_comento_info = {
    .name          = TYPE_GPIO_COMENTO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GPIO_COMENTO_State),
    .instance_init = gpio_comento_init,
};

static void gpio_comento_register_types(void)
{
    type_register_static(&gpio_comento_info);
}

type_init(gpio_comento_register_types)

