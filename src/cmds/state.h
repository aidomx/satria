#ifndef SATRIA_CMDS_STATE_MODE_H
#define SATRIA_CMDS_STATE_MODE_H

typedef enum { PROJECT, WORKSPACE } StateMode;
void stateSetMode(StateMode mode);
StateMode stateGetMode(void);

#endif
