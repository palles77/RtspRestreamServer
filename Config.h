#pragma once

#define STRING2(x) #x
#define STRING(x) STRING2(x)

#define STATIC_SERVER_PORT 8000
#define RESTREAM_SERVER_PORT 8001

#define STATIC_SERVER_PORT_STR STRING(STATIC_SERVER_PORT)
#define RESTREAM_SERVER_PORT_STR STRING(RESTREAM_SERVER_PORT)
