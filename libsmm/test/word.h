/*
 * word.h
 *
 *  Created on: Aug 4, 2015
 *      Author: xzl
 */

#ifndef WORD_H_
#define WORD_H_

void wc_fill_text(char *buf, int size);

int wc_init(void);

void wc_cleanup(void);

void consumer_wc_smp(char *buf, int size);

void consumer_wc_up(char *buf, int size);

#endif /* WORD_H_ */
