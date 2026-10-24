#pragma once
/* Diagnostic stream recorder. No cache or workspace-state dependency. */
int buildLogShow(void);
void buildLogBegin(void);
void buildLogEnd(int status);
