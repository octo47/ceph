#include "common/errno.h"
#include "include/rados/librados.h"
#include "test/rados-api/test.h"

#include "gtest/gtest.h"
#include <errno.h>
#include <semaphore.h>
#include <sstream>
#include <string>

using std::ostringstream;
using namespace librados;

class AioTestData
{
public:
  AioTestData()
    : m_init(false),
      m_complete(false),
      m_safe(false)
  {
  }

  ~AioTestData()
  {
    if (m_init) {
      rados_ioctx_destroy(m_ioctx);
      destroy_one_pool(m_pool_name, &m_cluster);
      sem_destroy(&m_sem);
    }
  }

  std::string init()
  {
    int ret;
    if (sem_init(&m_sem, 0, 0)) {
      int err = errno;
      sem_destroy(&m_sem);
      ostringstream oss;
      oss << "sem_init failed: " << cpp_strerror(err);
      return oss.str();
    }
    m_pool_name = get_temp_pool_name();
    std::string err = create_one_pool(m_pool_name, &m_cluster);
    if (!err.empty()) {
      sem_destroy(&m_sem);
      ostringstream oss;
      oss << "create_one_pool(" << m_pool_name << ") failed: error " << err;
      return oss.str();
    }
    ret = rados_ioctx_create(m_cluster, m_pool_name.c_str(), &m_ioctx);
    if (ret) {
      sem_destroy(&m_sem);
      destroy_one_pool(m_pool_name, &m_cluster);
      ostringstream oss;
      oss << "rados_ioctx_create failed: error " << ret;
      return oss.str();
    }
    m_init = true;
    return "";
  }

  sem_t m_sem;
  rados_t m_cluster;
  rados_ioctx_t m_ioctx;
  std::string m_pool_name;
  bool m_init;
  bool m_complete;
  bool m_safe;
};

class AioTestDataPP
{
public:
  AioTestDataPP()
    : m_init(false),
      m_complete(false),
      m_safe(false)
  {
  }

  ~AioTestDataPP()
  {
    if (m_init) {
      m_ioctx.close();
      destroy_one_pool_pp(m_pool_name, m_cluster);
      sem_destroy(&m_sem);
    }
  }

  std::string init()
  {
    int ret;
    if (sem_init(&m_sem, 0, 0)) {
      int err = errno;
      sem_destroy(&m_sem);
      ostringstream oss;
      oss << "sem_init failed: " << cpp_strerror(err);
      return oss.str();
    }
    m_pool_name = get_temp_pool_name();
    std::string err = create_one_pool_pp(m_pool_name, m_cluster);
    if (!err.empty()) {
      sem_destroy(&m_sem);
      ostringstream oss;
      oss << "create_one_pool(" << m_pool_name << ") failed: error " << err;
      return oss.str();
    }
    ret = m_cluster.ioctx_create(m_pool_name.c_str(), m_ioctx);
    if (ret) {
      sem_destroy(&m_sem);
      destroy_one_pool_pp(m_pool_name, m_cluster);
      ostringstream oss;
      oss << "rados_ioctx_create failed: error " << ret;
      return oss.str();
    }
    m_init = true;
    return "";
  }

  sem_t m_sem;
  Rados m_cluster;
  IoCtx m_ioctx;
  std::string m_pool_name;
  bool m_init;
  bool m_complete;
  bool m_safe;
};

void set_completion_complete(rados_completion_t cb, void *arg)
{
  AioTestData *test = (AioTestData*)arg;
  test->m_complete = true;
  sem_post(&test->m_sem);
}

void set_completion_safe(rados_completion_t cb, void *arg)
{
  AioTestData *test = (AioTestData*)arg;
  test->m_safe = true;
  sem_post(&test->m_sem);
}

TEST(LibRadosAio, SimpleWrite) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  TestAlarm alarm;
  sem_wait(&test_data.m_sem);
  sem_wait(&test_data.m_sem);
  rados_aio_release(my_completion);
}

TEST(LibRadosAio, SimpleWritePP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo",
			       my_completion, bl1, sizeof(buf), 0));
  TestAlarm alarm;
  sem_wait(&test_data.m_sem);
  sem_wait(&test_data.m_sem);
  delete my_completion;
}

TEST(LibRadosAio, WaitForSafe) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  TestAlarm alarm;
  ASSERT_EQ(0, rados_aio_wait_for_safe(my_completion));
  rados_aio_release(my_completion);
}

TEST(LibRadosAio, WaitForSafePP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo",
			       my_completion, bl1, sizeof(buf), 0));
  TestAlarm alarm;
  ASSERT_EQ(0, my_completion->wait_for_safe());
  delete my_completion;
}

TEST(LibRadosAio, RoundTrip) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    sem_wait(&test_data.m_sem);
    sem_wait(&test_data.m_sem);
  }
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST(LibRadosAio, RoundTripPP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo", my_completion,
					   bl1, sizeof(buf), 0));
  {
    TestAlarm alarm;
    sem_wait(&test_data.m_sem);
    sem_wait(&test_data.m_sem);
  }
  bufferlist bl2;
  AioCompletion *my_completion2 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion2, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("foo",
			      my_completion2, &bl2, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion2->wait_for_complete());
  }
  ASSERT_EQ(0, memcmp(buf, bl2.c_str(), sizeof(buf)));
  delete my_completion;
  delete my_completion2;
}

TEST(LibRadosAio, RoundTripAppend) {
  AioTestData test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  char buf2[128];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion2));
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  char buf3[sizeof(buf) + sizeof(buf2)];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(0, memcmp(buf3, buf, sizeof(buf)));
  ASSERT_EQ(0, memcmp(buf3 + sizeof(buf), buf2, sizeof(buf2)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST(LibRadosAio, RoundTripAppendPP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_append("foo", my_completion,
					    bl1, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion->wait_for_complete());
  }
  char buf2[128];
  memset(buf2, 0xdd, sizeof(buf2));
  bufferlist bl2;
  bl2.append(buf2, sizeof(buf2));
  AioCompletion *my_completion2 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion2, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_append("foo", my_completion2,
					    bl2, sizeof(buf2)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion2->wait_for_complete());
  }
  bufferlist bl3;
  AioCompletion *my_completion3 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion3, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("foo",
			      my_completion3, &bl3, 2 * sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion3->wait_for_complete());
  }
  ASSERT_EQ(0, memcmp(bl3.c_str(), buf, sizeof(buf)));
  ASSERT_EQ(0, memcmp(bl3.c_str() + sizeof(buf), buf2, sizeof(buf2)));
  delete my_completion;
  delete my_completion2;
  delete my_completion3;
}

TEST(LibRadosAio, IsComplete) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    sem_wait(&test_data.m_sem);
    sem_wait(&test_data.m_sem);
  }
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_complete.
    while (true) {
      int is_complete = rados_aio_is_complete(my_completion2);
      if (is_complete)
	break;
    }
  }
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST(LibRadosAio, IsCompletePP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo", my_completion,
					   bl1, sizeof(buf), 0));
  {
    TestAlarm alarm;
    sem_wait(&test_data.m_sem);
    sem_wait(&test_data.m_sem);
  }
  bufferlist bl2;
  AioCompletion *my_completion2 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion2, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("foo", my_completion2,
					  &bl2, sizeof(buf), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_complete.
    while (true) {
      int is_complete = my_completion2->is_complete();
      if (is_complete)
	break;
    }
  }
  ASSERT_EQ(0, memcmp(buf, bl2.c_str(), sizeof(buf)));
  delete my_completion;
  delete my_completion2;
}

TEST(LibRadosAio, IsSafe) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_safe.
    while (true) {
      int is_safe = rados_aio_is_safe(my_completion);
      if (is_safe)
	break;
    }
  }
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST(LibRadosAio, IsSafePP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo", my_completion,
					   bl1, sizeof(buf), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_safe.
    while (true) {
      int is_safe = my_completion->is_safe();
      if (is_safe)
	break;
    }
  }
  AioCompletion *my_completion2 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  bufferlist bl2;
  ASSERT_NE(my_completion2, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("foo", my_completion2,
					  &bl2, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion2->wait_for_complete());
  }
  ASSERT_EQ(0, memcmp(buf, bl2.c_str(), sizeof(buf)));
  delete my_completion;
  delete my_completion2;
}

TEST(LibRadosAio, ReturnValue) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0, sizeof(buf));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "nonexistent",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
}

TEST(LibRadosAio, ReturnValuePP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  bufferlist bl1;
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("nonexistent",
			       my_completion, &bl1, 128, 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion->wait_for_complete());
  }
  ASSERT_EQ(-ENOENT, my_completion->get_return_value());
  delete my_completion;
}

TEST(LibRadosAio, Flush) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xee, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  ASSERT_EQ(0, rados_aio_flush(test_data.m_ioctx));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST(LibRadosAio, FlushPP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xee, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo", my_completion,
					   bl1, sizeof(buf), 0));
  ASSERT_EQ(0, test_data.m_ioctx.aio_flush());
  bufferlist bl2;
  AioCompletion *my_completion2 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion2, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("foo", my_completion2,
					  &bl2, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion2->wait_for_complete());
  }
  ASSERT_EQ(0, memcmp(buf, bl2.c_str(), sizeof(buf)));
  delete my_completion;
  delete my_completion2;
}

TEST(LibRadosAio, RoundTripWriteFull) {
  AioTestData test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  ASSERT_EQ("", test_data.init());
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  char buf2[64];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion2));
  ASSERT_EQ(0, rados_aio_write_full(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf2)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  char buf3[sizeof(buf) + sizeof(buf2)];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion((void*)&test_data,
	      set_completion_complete, set_completion_safe, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(0, memcmp(buf3, buf2, sizeof(buf2)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST(LibRadosAio, RoundTripWriteFullPP) {
  AioTestDataPP test_data;
  ASSERT_EQ("", test_data.init());
  AioCompletion *my_completion = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl1;
  bl1.append(buf, sizeof(buf));
  ASSERT_EQ(0, test_data.m_ioctx.aio_write("foo", my_completion,
					   bl1, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion->wait_for_complete());
  }
  char buf2[64];
  memset(buf2, 0xdd, sizeof(buf2));
  bufferlist bl2;
  bl2.append(buf2, sizeof(buf2));
  AioCompletion *my_completion2 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion2, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_write_full("foo", my_completion2, bl2));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion2->wait_for_complete());
  }
  bufferlist bl3;
  AioCompletion *my_completion3 = test_data.m_cluster.aio_create_completion(
	  (void*)&test_data, set_completion_complete, set_completion_safe);
  ASSERT_NE(my_completion3, my_completion_null);
  ASSERT_EQ(0, test_data.m_ioctx.aio_read("foo", my_completion3,
					  &bl3, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, my_completion3->wait_for_complete());
  }
  ASSERT_EQ(0, memcmp(bl3.c_str(), buf2, sizeof(buf2)));
  delete my_completion;
  delete my_completion2;
  delete my_completion3;
}
