import argparse
import minio


def create_bucket(minio_endpoint_url, minio_access_key, minio_secret_key, bucket_name):
    secure = False
    minio_endpoint = ""
    if minio_endpoint_url.find("http://") == 0:
        secure = False
        minio_endpoint = minio_endpoint_url.removeprefix("http://")
    elif minio_endpoint_url.find("https://") == 0:
        secure = True
        minio_endpoint = minio_endpoint_url.removeprefix("https://")

    client = minio.Minio(minio_endpoint,
                         access_key=minio_access_key,
                         secret_key=minio_secret_key,
                         secure=secure)

    # create bucket
    if not client.bucket_exists(bucket_name):
        client.make_bucket(bucket_name)
        print(f"minio bucket#{bucket_name} has been created.")
    else:
        print(f"minio bucket#{bucket_name} already exists")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--minio_endpoint", required=True, help="Endpoint of minio s3.")
    parser.add_argument("--minio_access_key", required=True, help="AccessKey of minio s3.")
    parser.add_argument("--minio_secret_key", required=True, help="SecretKey of minio s3.")
    parser.add_argument("--bucket_name", required=True, help="Name of bucket to create.")

    parsed_args = parser.parse_args()

    create_bucket(minio_endpoint_url=parsed_args.minio_endpoint,
             minio_access_key=parsed_args.minio_access_key,
             minio_secret_key=parsed_args.minio_secret_key,
             bucket_name=parsed_args.bucket_name)
