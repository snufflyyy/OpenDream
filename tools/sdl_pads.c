/* Lists SDL3 gamepads with their motion sensors and prints a few gyro/accel samples (pad motion
 * diagnostics, outside the game). Build: gcc tools/sdl_pads.c $(pkg-config --cflags --libs sdl3) -o port/out/tools/sdl-pads */
#include <SDL3/SDL.h>
#include <stdio.h>
int main(void){
  SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS,"1");
  if(!SDL_Init(SDL_INIT_GAMEPAD)){printf("init %s\n",SDL_GetError());return 1;}
  SDL_Delay(500); SDL_UpdateGamepads();
  int n=0; SDL_JoystickID *ids=SDL_GetGamepads(&n);
  for(int i=0;i<n;i++){ SDL_Gamepad*g=SDL_OpenGamepad(ids[i]);
    printf("%d id=%u name=%s path=%s type=%d gyro=%d accel=%d\n",i,ids[i],SDL_GetGamepadName(g),SDL_GetGamepadPath(g),SDL_GetGamepadType(g),
      SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO),SDL_GamepadHasSensor(g,SDL_SENSOR_ACCEL));
    if(SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_GYRO,true)&&SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_ACCEL,true)){
      for(int k=0;k<5;k++){SDL_Delay(50);SDL_UpdateGamepads();float gy[3],ac[3];SDL_GetGamepadSensorData(g,SDL_SENSOR_GYRO,gy,3);SDL_GetGamepadSensorData(g,SDL_SENSOR_ACCEL,ac,3);
        printf("  gyro %.3f %.3f %.3f accel %.2f %.2f %.2f\n",gy[0],gy[1],gy[2],ac[0],ac[1],ac[2]);}
    }
  }
  return 0;}
