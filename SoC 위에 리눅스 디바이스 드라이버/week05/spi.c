#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/spi/spi.h> // SPI 관련 API를 사용하기 위해 include 함

#define COMENTO_SPI_GET_SCALE 1
#define COMENTO_SPI_SET_ZERO 2

struct comento_spi_device {
    struct spi_device *dev;
    struct spi_message msg; // 어떤 식으로 통신할 지 미리 정해두어야 함
    struct spi_transfer xfer[2]; // 메시지에 포함 될 전송 방법
    char tx_buf[1];    // 송신용 데이터 버퍼
    char rx_buf[2];    // 수신용 데이터 버퍼
    //송신할 데이터는 명령어코드 1바이트, 수신할 데이터는 반환값 2바이트
    struct mutex lock;
};

static ssize_t comento_show_scale(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{ // sysfs 파일을 읽었을 때 호출되는 함수, 문자열을 buf로 출력하기만 하면
    ssize_t ret;                       // 사용자 공간까지 자동으로 전달됨
    short val;
    struct comento_spi_device *csdev = dev_get_drvdata(dev);
    // 통신하는 동안 또 통신을 시도하면 안되므로 뮤텍스로 보호
    mutex_lock(&csdev->lock);
    csdev->tx_buf[0] = COMENTO_SPI_GET_SCALE;// 송신버퍼에 명령어코드 추가
    // spi_sync라는 API는 메시지에 명시된 대로 SPI 통신을 수행
    ret = spi_sync(csdev->dev, &csdev->msg);
    if (ret < 0) {
        printk(KERN_ERR "comento_spi: spi_sync failed - %d\n", ret);
        mutex_unlock(&csdev->lock);
        return ret;
    } // spi_sync는 SPI 통신을 수행하고 수신버퍼에 데이터를 넣어줌
    // 수신버퍼에 들어간 2byte 데이터를 short 타입으로 변환
    val = (csdev->rx_buf[0] << 8) | csdev->rx_buf[1];
    mutex_unlock(&csdev->lock);
    return sprintf(buf, "%d\n", val); // sprintf로 문자열로 buf에 넣어줌
}

static ssize_t comento_store_zero(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t len)
{// sysfs 파일에 쓸 때 호출되는 함수, buf에 써진 내용이 자동으로 전달됨
    ssize_t ret;
    struct comento_spi_device *csdev = dev_get_drvdata(dev);

    if (strcmp(buf, "zero\n")) return len; //”zero\n”라고 썼을때만 동작
    mutex_lock(&csdev->lock);
    csdev->tx_buf[0] = COMENTO_SPI_SET_ZERO; // 송신버퍼에 명령어코드 추가
    ret = spi_sync(csdev->dev, &csdev->msg); // spi_sync로 SPI 통신을 수행
    if (ret < 0) {
        printk(KERN_ERR "comento_spi: spi_sync failed - %d\n", ret);
        mutex_unlock(&csdev->lock);
        return ret;
    }
    mutex_unlock(&csdev->lock);
    return len;
}
// sysfs로 만들 파일 정의, scale이라는 이름의 664 권한의 파일을 만듬
DEVICE_ATTR(scale, 0664, comento_show_scale, comento_store_zero);
static struct attribute *comento_spi_attributes[] = {
        &dev_attr_scale.attr,
        NULL,
};
static const struct attribute_group comento_spi_attr_group = {
        .attrs  = comento_spi_attributes,
};

// SPI 주변장치가 디바이스 트리에 명시되어 탐지 되었을 때 호출 됨
static int comento_spi_probe(struct spi_device *dev)
{    struct comento_spi_device *csdev;
    int err;

    dev->bits_per_word = 8; // 주변장치는 한번에 1바이트씩 통신
    dev->mode = SPI_MODE_0; // 주변장치가 사용할 SPI 처리 방법
    // SPI_MODE_2은 SCK 클럭의 라이징 엣지에 비트를 처리함을 의미
    // 하드웨어 구현에 따라 다른 부분 (QEMU에서는 신경 쓰지 않아도 됨)
    err = spi_setup(dev);
    if (err < 0) return err;

    csdev = devm_kzalloc(&dev->dev, sizeof(struct comento_spi_device),
                         GFP_KERNEL);
    if (csdev == NULL) return -ENOMEM;
        // 어떤식으로 통신할지 명시하는 메시지 구조체를 초기화
        spi_message_init(&csdev->msg);

        csdev->xfer[0].tx_buf = csdev->tx_buf; // 처음에는 송신버퍼만 사용
        csdev->xfer[0].len = 1;                // 송신 크기는 1바이트
        // 메시지에 전송방법 추가
        spi_message_add_tail(&csdev->xfer[0], &csdev->msg);

        csdev->xfer[1].rx_buf = csdev->rx_buf; // 처음에는 수신버퍼만 사용
        csdev->xfer[1].len = 2;                // 수신 크기는 2바이트
        // 메시지에 전송방법 추가
        spi_message_add_tail(&csdev->xfer[1], &csdev->msg);

        // 메시지에 2가지 전송 방법이 추가 된 것 :
        // 첫번째는 송신 버퍼의 1바이트 송신,
        // 두번째는 수신 버퍼로 2바이트 수신

        mutex_init(&csdev->lock);

        csdev->dev = dev;
        // 나중에 spi_get_drvdata로 얻어오기 위해 추가
        spi_set_drvdata(dev, csdev); 
        sysfs_create_group(&dev->dev.kobj, &comento_spi_attr_group);

	return 0;
}
// 드라이버 모듈이 언로드 되면 호출 됨
static void comento_spi_remove(struct spi_device *spi)
{
    struct comento_spi_device *csdev = spi_get_drvdata(spi);
    mutex_destroy(&csdev->lock);
    sysfs_remove_group(&spi->dev.kobj, &comento_spi_attr_group);
}
static const struct spi_device_id comento_spi_ids[] = {
    { "comento-spi", 0 }, // 이름을 여러 개 명시도 가능
    { },
}; // 디바이스트리의 compatible에서 사용할 드라이버의 이름 명시

// SPIDEV를 초기화할 때 compatible에 comento_spi_ids에 지정한
// 이름이 있다면 이 드라이버를 초기화 하도록 설정
MODULE_DEVICE_TABLE(spi, comento_spi_ids); 

static struct spi_driver comento_spi_driver = {
    .driver     = {
        // 드라이버의 이름으로 compatible과는 무관함
        .name   = "comento-spi",
    },
    .id_table   = comento_spi_ids,
    .probe      = comento_spi_probe,
    .remove     = comento_spi_remove,
};

// 지금 드라이버가 SPI 주변장치를 위한 것임을 명시
module_spi_driver(comento_spi_driver);

MODULE_AUTHOR("DDunAnt<ddunant@comento.com");
MODULE_DESCRIPTION("Comento SPI device Driver");
MODULE_LICENSE("GPL");

