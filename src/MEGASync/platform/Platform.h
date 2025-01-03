#ifndef PLATFORM_H
#define PLATFORM_H

#include "AbstractPlatform.h"

class Platform
{
public:
    Platform() = delete;
    ~Platform() = delete;

    static void create();
    static void destroy();
    static AbstractPlatform* getInstance();


private:
    static AbstractPlatform* mInstance;
};

#endif // PLATFORM_H
