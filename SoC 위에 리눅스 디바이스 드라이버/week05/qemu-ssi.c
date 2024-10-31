#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "sysemu/runstate.h"
#include "qapi/visitor.h"
#define TYPE_SSI_COMENTO "ssi-comento"
#define COMENTO_SSI_GET_SCALE  1 // 명령어 코드를 2개 정의 :
#define COMENTO_SSI_SET_ZERO   2 // 현재 무게 얻어오기, 영점 조정하기

OBJECT_DECLARE_SIMPLE_TYPE(SSI_COMENTO_State, SSI_COMENTO)
struct SSI_COMENTO_State {
    SSIPeripheral parent_obj;
    uint16_t scale;
    uint16_t offset;
    int cycle;
};
// SPI 통신을 처리하기 위한 함수
// data 파라미터로는 MOSI의 입력, 반환 값은 MISO로 출력
// 명령어 코드로 1바이트, 반환 값으로 2바이트를 사용하는 구현
static uint32_t ssi_comento_transfer(SSIPeripheral *obj, uint32_t data)
{
    SSI_COMENTO_State *s = SSI_COMENTO(obj);
    if (data != 0) {
        // 영점 조정을 위해 COMENTO_SSI_SET_ZERO 명령어를 보낸 경우
        if (data == COMENTO_SSI_SET_ZERO) {
            s->offset = s->scale; // 영점을 현재 무게 값으로 조정
        }
        s->cycle = 0; // 명령어를 뭔가 받았다면 현재 cycle을 0번으로 설정
    } else {
        s->cycle++;   // 명령어를 받지 않았다면 cycle 증가
    }

    if (s->cycle == 1) { // 1번 사이클이면 scale의 상위 8비트 반환
        return (uint8_t)((s->scale - s->offset) >> 8);
    } else if (s->cycle == 2) { // 2번 사이클이면 scale의 하위 8비트 반환
        return (uint8_t)(s->scale - s->offset);
    }
    return 0; // 0번 사이클에는 데이터를 0으로 보냄
}
// QMP로 scale 속성을 얻어오려고 할 때 문자열로 반환
static char *ssi_comento_get_scale(Object *obj, Error **errp)
{
    SSI_COMENTO_State *s = SSI_COMENTO(obj);
    char buf[8];
    sprintf(buf, "%d\n", s->scale);
    return g_strdup(buf);
}
// QMP로 scale 속성을 문자열로 넣었을 때 값 갱신
static void ssi_comento_set_scale(Object *obj, const char *value,
                                  Error **errp)
{
    SSI_COMENTO_State *s = SSI_COMENTO(obj);
    sscanf(value, "%hd", &s->scale);
}

static void ssi_comento_init(Object *obj)
{
    object_property_add_str(obj, "scale",
                    ssi_comento_get_scale, ssi_comento_set_scale);
}
// 버스에 연결되자 마자 해야할 일은 없으므로 빈 함수
static void ssi_comento_realize(SSIPeripheral *obj, Error **errp)
{
}
static void ssi_comento_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->cs_polarity = SSI_CS_HIGH; // 칩선택이 High 일 때 동작하도록 설정
    k->realize = ssi_comento_realize;
    k->transfer = ssi_comento_transfer;
}

static const TypeInfo ssi_comento_info = {
    .name          = TYPE_SSI_COMENTO,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SSI_COMENTO_State),
    .instance_init = ssi_comento_init,
    .class_init = ssi_comento_class_init,
};

static void ssi_comento_register_types(void)
{
    type_register_static(&ssi_comento_info);
}

type_init(ssi_comento_register_types)

