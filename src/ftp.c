/***************************************************************************/
/*                                                                         */
/* ftp.c - mruby gem provoding ftp access                                  */
/* Copyright (C) 2015 Paolo Bosetti and Matteo Ragni,                      */
/* paolo[dot]bosetti[at]unitn.it and matteo[dot]ragni[at]unitn.it          */
/* Department of Industrial Engineering, University of Trento              */
/*                                                                         */
/* This library is free software.  You can redistribute it and/or          */
/* modify it under the terms of the GNU GENERAL PUBLIC LICENSE 2.0.        */
/*                                                                         */
/* This library is distributed in the hope that it will be useful,         */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of          */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           */
/* Artistic License 2.0 for more details.                                  */
/*                                                                         */
/* See the file LICENSE                                                    */
/*                                                                         */
/***************************************************************************/

/*
This gem uses Thomas Pfau's FTPlib library, for documentation and license
see http://nbpfaus.net/~pfau/ftplib/ftplib.html
*/

#include <stdlib.h>
#include <string.h>
#include "mruby.h"
#include "mruby/variable.h"
#include "mruby/string.h"
#include "mruby/data.h"
#include "mruby/class.h"
#include "mruby/value.h"
#include "ftplib.h"

enum mruby_ftp_state {
  FTP_STATE_TO_INIT = -1, // -1 -> Pseudo state (need initialization)
  FTP_STATE_CLOSED,       //  0 -> State on mrb_ftp_data_init and mrb_ftp_close
  FTP_STATE_CONNECTED,    //  1 -> State on mrb_ftp_connect
  FTP_STATE_LOGGED_IN     //  2 -> Sate on mrb_ftp_login
};

enum mruby_ftp_xfer {
  FTP_XFER_TEXT = 0, // 0 - performs ascii transfer
  FTP_XFER_BINARY    // 1 - performs binary transfer
};

// Container for netbuf struct and a state variable that identifies the
// actual state of the ftp server.
struct netbuf_data {
  netbuf *conn;
  char state;
};

// FIXME :: substitute with representation of maximum string length
#define MAX_STRING_LENGTH 2048

// TODO: Check for portability, esp. on M$
#ifdef _WIN32
#warning Untested platform identified!
#define CONSOLE_STREAM "CON"
#define NULL_STREAM "NUL"
#else
#define CONSOLE_STREAM "/dev/tty"
#define NULL_STREAM "/dev/null"
#endif

#define GRAB_STDOUT(stream_str, chain)                                         \
  stream_str = (char *)malloc(MAX_STRING_LENGTH * sizeof(char));               \
  fflush(stdout);                                                              \
  freopen(NULL_STREAM, "a", stdout);                                           \
  setbuffer(stdout, stream_str, MAX_STRING_LENGTH);                            \
  chain;                                                                       \
  freopen(CONSOLE_STREAM, "a", stdout);                                        \
  fflush(stdout);

#define FTPLIB_SUCCEED 1
#define FTPLIB_ERROR 0

#define ALREADY_LOGIN_STATE_RAISE                                              \
  switch (data->state) {                                                       \
  case FTP_STATE_CLOSED:                                                       \
    mrb_raise(mrb, E_RUNTIME_ERROR, "Not connected to server");                \
    break;                                                                     \
  case FTP_STATE_CONNECTED:                                                    \
    mrb_raise(mrb, E_RUNTIME_ERROR, "Not logged in");                          \
    break;                                                                     \
  default:                                                                     \
    mrb_raise(mrb, E_RUNTIME_ERROR, "Undefined state, cannot continue");       \
    break;                                                                     \
  }

// Garbage collector handler, for netbuf_data struct
static void netbuf_data_destructor(mrb_state *mrb, void *p_) { free(p_); };

// Macro loads from istanced object the value contained in
// @data var, that is actually the netbuf for a specif instance
// Obtains a pointer to allocated struct.
#define CONNECTION_DATA_STRUCT                                                 \
  DATA_GET_PTR(mrb, mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@data")),      \
               &netbuf_data_type, struct netbuf_data);

// Check routines on @data instance variable
// Return true if @data is nil
#define CHECK_DATA_NIL                                                         \
  mrb_nil_p(mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@data")))
// Raise an error if @data is nil
#define CHECK_DATA_EXISTENCE                                                   \
  if (CHECK_DATA_NIL) {                                                        \
    mrb_raise(mrb, E_RUNTIME_ERROR, "Undefined @state. Use FTP#open");         \
  }

// Creating data type and reference for GC, in a const struct
const struct mrb_data_type netbuf_data_type = {"netbuf_data",
                                               netbuf_data_destructor};

// FTP Class methods interface functions

/*
char xfer_mode(mrb_int);
static mrb_value mrb_ftp_data_init(mrb_state *, mrb_value);
static mrb_value mrb_ftp_connect(mrb_state *, mrb_value);
static mrb_value mrb_ftp_login(mrb_state *, mrb_value);

static mrb_value mrb_ftp_chdir(mrb_state *, mrb_value);
static mrb_value mrb_ftp_mkdir(mrb_state *, mrb_value);
static mrb_value mrb_ftp_rmdir(mrb_state *, mrb_value);
static mrb_value mrb_ftp_dir(mrb_state *, mrb_value);
static mrb_value mrb_ftp_pwd(mrb_state *, mrb_value);

static mrb_value mrb_ftp_put(mrb_state *, mrb_value);
static mrb_value mrb_ftp_get(mrb_state *, mrb_value);
static mrb_value mrb_ftp_delete(mrb_state *, mrb_value);
static mrb_value mrb_ftp_rename(mrb_state *, mrb_value);

static mrb_value mrb_ftp_close(mrb_state *, mrb_value);
static mrb_value mrb_ftp_lastmessage(mrb_state *, mrb_value);
static mrb_value mrb_ftp_state(mrb_state *, mrb_value);
*/
// FTP Class implementations

char xfer_mode(mrb_int in) {
  switch (in) {
  case FTP_XFER_TEXT:
    return (char)FTPLIB_TEXT;
    break;
  case FTP_XFER_BINARY:
    return (char)FTPLIB_BINARY;
    break;
  default:
    return (char)FTPLIB_TEXT;
    break;
  }
}

static mrb_value mrb_ftp_data_init(mrb_state *mrb, mrb_value self) {
  // Check id @data is nil
  if (CHECK_DATA_NIL) {
    // Create a new netbuf_data struct and save in class istance
    struct netbuf_data *data = malloc(sizeof(struct netbuf_data));
    if (data) {
      struct RClass *c = mrb_class_ptr(self);
      mrb_iv_set(
          mrb, self, mrb_intern_cstr(mrb, "@data"),
          mrb_obj_value(Data_Wrap_Struct(mrb, c, &netbuf_data_type, data)));
      data->state = FTP_STATE_CLOSED;
      return mrb_true_value();
    } else {
      // Raise an error when it cannot allocate
      mrb_raise(mrb, E_RUNTIME_ERROR, "Could not allocate @data");
    }
  } else {
    return mrb_false_value();
  }
}

// Connect Function. The function will instanciate a netbuf_data struct
static mrb_value mrb_ftp_connect(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Rescue in case of not initialized @data not initialized.
  if (CHECK_DATA_NIL) {
    mrb_ftp_data_init(mrb, self);
  }

  // Loading data and making actual login
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->state == FTP_STATE_CLOSED) {
      // Loading @hostname, as required value
      mrb_value hostname =
          mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@hostname"));
      const char *host = mrb_str_to_cstr(mrb, hostname);
      // Execute connection
      if (FtpConnect(host, &data->conn) == FTPLIB_ERROR) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "Could not connect");
      }
      data->state = FTP_STATE_CONNECTED;
      return self;
    } else {
      // Raise an error if state is not closed
      switch (data->state) {
      case FTP_STATE_CONNECTED:
        mrb_raise(mrb, E_RUNTIME_ERROR, "Already connected to remote server");
        break;
      case FTP_STATE_LOGGED_IN:
        mrb_raise(mrb, E_RUNTIME_ERROR, "Already logged on remote server");
        break;
      default:
        mrb_raise(mrb, E_RUNTIME_ERROR, "Undefined state, cannot connect");
        break;
      }
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_login(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE

  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->state == FTP_STATE_CONNECTED) {
      mrb_value user = mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@user"));
      mrb_value pwd = mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@pwd"));
      const char *pUser = mrb_str_to_cstr(mrb, user);
      const char *pPwd = mrb_str_to_cstr(mrb, pwd);
      // Executes login function
      if (data->conn) {
        if (FtpLogin(pUser, pPwd, data->conn) == FTPLIB_SUCCEED) {
          // if succeed changes state and
          data->state = FTP_STATE_LOGGED_IN;
          return self;
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot log in");
        }
      } else {
        // ftp state well defined but data->conn not
        mrb_raise(mrb, E_RUNTIME_ERROR,
                  "Unknown Error (unable to read connection internal state)");
      }
    } else {
      // Handles error for other state
      switch (data->state) {
      case FTP_STATE_CLOSED:
        mrb_raise(mrb, E_RUNTIME_ERROR,
                  "Not connected, cannot login. Use FTP#open");
        break;
      case FTP_STATE_LOGGED_IN:
        mrb_raise(mrb, E_RUNTIME_ERROR, "Already logged on remote server");
        break;
      default:
        mrb_raise(mrb, E_RUNTIME_ERROR, "Undefined state, cannot connect");
        break;
      }
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_pwd(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        // FIXME :: dinamically allocated string? Is the best way?
        char *pPwd = (char *)malloc(MAX_STRING_LENGTH * sizeof(char));
        if (pPwd) {
          if (FtpPwd(pPwd, MAX_STRING_LENGTH, data->conn) == FTPLIB_SUCCEED) {
            mrb_value rv = mrb_str_new_cstr(mrb, pPwd);
            free(pPwd);
            return rv;
          } else {
            free(pPwd);
            mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot execute PWD");
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute pwd (pPwd malloc failed)");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_chdir(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *dest_name;
        mrb_int len;
        mrb_get_args(mrb, "s", &dest_name, &len);
        if (FtpChdir((const char *)dest_name, data->conn) == FTPLIB_SUCCEED) {
          return mrb_ftp_pwd(mrb, self);
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot execute CD");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_cdup(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        if (FtpCDUp(data->conn) == FTPLIB_SUCCEED) {
          return mrb_ftp_pwd(mrb, self);
        } else {
          return mrb_false_value();
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_mkdir(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *dir_name;
        mrb_int dir_len;
        mrb_get_args(mrb, "s", &dir_name, &dir_len);
        if (dir_name) {
          if (FtpMkdir((const char *)dir_name, data->conn) == FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute MKDIR. Error reading dir_name");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_rmdir(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *dir_name;
        mrb_int dir_len;
        mrb_get_args(mrb, "s", &dir_name, &dir_len);
        if (dir_name) {
          if (FtpRmdir((const char *)dir_name, data->conn) == FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute RMDIR. Error reading dir_name");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_dir(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        // Loading arguments
        char *dest_name = (char *)NULL;
        mrb_int len;
        char *ret_str;
        int result = 0;
        mrb_get_args(mrb, "|s", &dest_name, &len);
        if (!dest_name) {
          dest_name = (char *)malloc(5 * sizeof(char));
          strncpy(dest_name, ".", 4);
        }
        // Executing command
        GRAB_STDOUT(ret_str,
                    result = FtpDir((const char *)NULL, dest_name, data->conn));
        // Results check
        if (result == FTPLIB_SUCCEED) {
          mrb_value rv = mrb_str_new_cstr(mrb, ret_str);
          free(ret_str);
          if (!dest_name)
            free(dest_name);
          return rv;
        } else {
          free(ret_str);
          if (!dest_name)
            free(dest_name);
          mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot execute DIR");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_nlst(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        // Loading arguments
        char *dest_name = (char *)NULL;
        mrb_int len;
        mrb_get_args(mrb, "|s", &dest_name, &len);
        if (!dest_name) {
          dest_name = (char *)malloc(5 * sizeof(char));
          strncpy(dest_name, ".", 4);
        }
        // Executing command
        char *ret_str;
        int result = 0;
        GRAB_STDOUT(ret_str, result = FtpNlst((const char *)NULL, dest_name,
                                              data->conn));
        // Results check
        if (result == FTPLIB_SUCCEED) {
          mrb_value rv = mrb_str_new_cstr(mrb, ret_str);
          free(ret_str);
          if (!dest_name)
            free(dest_name);
          return rv;
        } else {
          free(ret_str);
          if (!dest_name)
            free(dest_name);
          mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot execute NLST");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_put(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *src_path, *dest_path;
        mrb_int src_len, dest_len, mode;
        mrb_get_args(mrb, "ssi", &src_path, &src_len, &dest_path, &dest_len,
                     &mode);
        if (src_path && dest_path) {
          if (FtpPut((const char *)src_path, (const char *)dest_path,
                     xfer_mode(mode), data->conn) == FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute PUT. Error reading src_path or dest_path");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_get(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *src_path, *dest_path;
        mrb_int src_len, dest_len, mode;
        mrb_get_args(mrb, "ssi", &src_path, &src_len, &dest_path, &dest_len,
                     &mode);
        if (src_path && dest_path) {
          if (FtpGet((const char *)dest_path, (const char *)src_path,
                     xfer_mode(mode), data->conn) == FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute GET. Error reading src_path or dest_path");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_delete(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *file_path;
        mrb_int file_len;
        mrb_get_args(mrb, "s", &file_path, &file_len);
        if (file_path) {
          if (FtpDelete((const char *)file_path, data->conn) ==
              FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute DELETE. Error reading file_path");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_rename(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *src_path, *dest_path;
        mrb_int src_len, dest_len;
        mrb_get_args(mrb, "ss", &src_path, &src_len, &dest_path, &dest_len);
        if (src_path && dest_path) {
          if (FtpRename((const char *)src_path, (const char *)dest_path,
                        data->conn) == FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(
              mrb, E_RUNTIME_ERROR,
              "Cannot execute RENAME. Error reading src_path or dest_path");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

// FIXME :: Not tested!
static mrb_value mrb_ftp_size(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *file_path;
        mrb_int file_len;
        unsigned int file_size;
        mrb_get_args(mrb, "s", &file_path, &file_len);
        if (file_path) {
          if (FtpSize((const char *)file_path, &file_size, FTPLIB_ASCII,
                      data->conn) == FTPLIB_SUCCEED) {
            return mrb_fixnum_value(file_size);
          } else {
            return mrb_nil_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute DELETE. Error reading file_path");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_close(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      // Quits from server no matter what the state!
      FtpQuit(data->conn);
      data->state = FTP_STATE_CLOSED;
      return mrb_true_value();
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_lastmessage(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      char *pMsg = FtpLastResponse(data->conn);
      return mrb_str_new_cstr(mrb, pMsg);
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_state(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Return to be initialize if it finds @data = nil
  if (CHECK_DATA_NIL) {
    return mrb_fixnum_value(FTP_STATE_TO_INIT);
  }
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    return mrb_fixnum_value(data->state);
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

static mrb_value mrb_ftp_site(mrb_state *mrb, mrb_value self) {
  struct netbuf_data *data;
  // Error in case of not initialized @data not initialized.
  CHECK_DATA_EXISTENCE
  data = CONNECTION_DATA_STRUCT;
  if (data) {
    if (data->conn) {
      if (data->state == FTP_STATE_LOGGED_IN) {
        char *arg_str;
        mrb_int file_len;
        mrb_get_args(mrb, "s", &arg_str, &file_len);
        if (arg_str) {
          if (FtpSite((const char *)arg_str, data->conn) == FTPLIB_SUCCEED) {
            return mrb_true_value();
          } else {
            return mrb_false_value();
          }
        } else {
          mrb_raise(mrb, E_RUNTIME_ERROR,
                    "Cannot execute SITE. Error reading arg_str");
        }
      } else {
        ALREADY_LOGIN_STATE_RAISE
      }
    } else {
      // ftp state defined but not data->conn
      mrb_raise(mrb, E_RUNTIME_ERROR,
                "Unknown Error (unable to read connection internal state)");
    }
  } else {
    // Raise an error if it cannot load data
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot load @data");
  }
}

/* ------------------------------------------------------------------------*/
void mrb_mruby_ftp_gem_init(mrb_state *mrb) {
  struct RClass *ftp;
  ftp = mrb_define_class(mrb, "FTP", mrb->object_class);
  FtpInit();
  mrb_define_method(mrb, ftp, "data_init", mrb_ftp_data_init, MRB_ARGS_NONE());

  mrb_define_method(mrb, ftp, "open", mrb_ftp_connect, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, ftp, "login", mrb_ftp_login, MRB_ARGS_NONE());

  mrb_define_method(mrb, ftp, "chdir", mrb_ftp_chdir, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, ftp, "cdup", mrb_ftp_cdup, MRB_ARGS_NONE());
  mrb_define_method(mrb, ftp, "mkdir", mrb_ftp_mkdir, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, ftp, "rmdir", mrb_ftp_rmdir, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, ftp, "dir", mrb_ftp_dir, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, ftp, "nlst", mrb_ftp_nlst, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, ftp, "pwd", mrb_ftp_pwd, MRB_ARGS_NONE());

  mrb_define_method(mrb, ftp, "put", mrb_ftp_put, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, ftp, "get", mrb_ftp_get, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, ftp, "delete", mrb_ftp_delete, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, ftp, "rename", mrb_ftp_rename, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, ftp, "size", mrb_ftp_size, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, ftp, "close", mrb_ftp_close, MRB_ARGS_NONE());

  mrb_define_method(mrb, ftp, "last_message", mrb_ftp_lastmessage,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, ftp, "state", mrb_ftp_state, MRB_ARGS_NONE());

  mrb_define_method(mrb, ftp, "site", mrb_ftp_site, MRB_ARGS_REQ(1));
}

void mrb_mruby_ftp_gem_final(mrb_state *mrb) {}
