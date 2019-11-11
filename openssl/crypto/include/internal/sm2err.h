/*
 * Generated by util/mkerr.pl DO NOT EDIT
 * Copyright 1995-2018 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#ifndef HEADER_SM2ERR_H
# define HEADER_SM2ERR_H

# include <openssl/opensslconf.h>

# ifndef OPENSSL_NO_SM2

#  ifdef  __cplusplus
extern "C"
#  endif
int ERR_load_SM2_strings(void);

/*
 * SM2 function codes.
 */
#  define SM2_F_PKEY_SM2_COPY                              115
#  define SM2_F_PKEY_SM2_CTRL                              109
#  define SM2_F_PKEY_SM2_CTRL_STR                          110
#  define SM2_F_PKEY_SM2_DIGEST_CUSTOM                     114
#  define SM2_F_PKEY_SM2_INIT                              111
#  define SM2_F_PKEY_SM2_SIGN                              112
#  define SM2_F_SM2_COMPUTE_MSG_HASH                       100
#  define SM2_F_SM2_COMPUTE_USERID_DIGEST                  101
#  define SM2_F_SM2_COMPUTE_Z_DIGEST                       113
#  define SM2_F_SM2_DECRYPT                                102
#  define SM2_F_SM2_ENCRYPT                                103
#  define SM2_F_SM2_PLAINTEXT_SIZE                         104
#  define SM2_F_SM2_SIGN                                   105
#  define SM2_F_SM2_SIG_GEN                                106
#  define SM2_F_SM2_SIG_VERIFY                             107
#  define SM2_F_SM2_VERIFY                                 108

/*
 * SM2 reason codes.
 */
#  define SM2_R_ASN1_ERROR                                 100
#  define SM2_R_BAD_SIGNATURE                              101
#  define SM2_R_BUFFER_TOO_SMALL                           107
#  define SM2_R_DIST_ID_TOO_LARGE                          110
#  define SM2_R_ID_NOT_SET                                 112
#  define SM2_R_ID_TOO_LARGE                               111
#  define SM2_R_INVALID_CURVE                              108
#  define SM2_R_INVALID_DIGEST                             102
#  define SM2_R_INVALID_DIGEST_TYPE                        103
#  define SM2_R_INVALID_ENCODING                           104
#  define SM2_R_INVALID_FIELD                              105
#  define SM2_R_NO_PARAMETERS_SET                          109
#  define SM2_R_USER_ID_TOO_LARGE                          106

# endif
#endif
