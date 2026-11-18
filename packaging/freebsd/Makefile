PORTNAME=	blockwright
DISTVERSION=	0.1.0
CATEGORIES=	graphics
MASTER_SITES=	https://git.pumpin.online/funkpower/blockwright/releases/download/v${DISTVERSION}/

COMMENT=	Editor for ANSI and IRC block art
WWW=		https://git.pumpin.online/funkpower/blockwright

LICENSE=	GPLv3+ CC-BY-SA-4.0 MIT OFL11
LICENSE_COMB=	multi
LICENSE_FILE_GPLv3+ =	${WRKSRC}/LICENSE
LICENSE_FILE_CC-BY-SA-4.0=	${WRKSRC}/third_party/oldschool-pc-fonts/LICENSE
LICENSE_FILE_MIT=	${WRKSRC}/third_party/tabler-icons/LICENSE
LICENSE_FILE_OFL11=	${WRKSRC}/third_party/unifont/OFL-1.1.txt

BUILD_DEPENDS=	zstd>0:archivers/zstd
LIB_DEPENDS=	libzip.so:archivers/libzip
TEST_DEPENDS=	catch2>=3:devel/catch2 \
		noto-basic>0:x11-fonts/noto-basic

USES=		cmake:testing compiler:c++20-lang gl python:test qt:6
USE_GL=		opengl
USE_QT=		base svg

CMAKE_OFF=	BUILD_TESTING BW_FETCH_CATCH2
TEST_ENV=	QT_QPA_PLATFORM=offscreen

PORTDATA=	help library samples

.include <bsd.port.mk>
